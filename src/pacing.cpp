// Frame pacing diagnostics (see pacing.h). Every thread is measured separately (thread-local accumulators); the
// status line lists the render thread and the busiest other threads, to find where the 60 fps limit lives.
#include "pacing.h"
#include "globals.h"
#include "MinHook.h"
#include "xrbridge/client.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <string>
#include <map>
#include <vector>
#include <algorithm>

namespace fvr {

static DWORD g_renderTid = 0;
static LARGE_INTEGER g_freq;
static bool g_installed = false;
// frame limiter thread (see Hk_TSE) and its clock: limiter time = scaledBase + (real - realBase) * scale.
// Changing the scale re-anchors the bases so the limiter's clock never jumps.
static volatile DWORD g_limiterTid = 0;
static double g_qpcScale = 1.0;
static LONGLONG g_qpcRealBase = 0, g_qpcScaledBase = 0;
static SRWLOCK g_qpcLock = SRWLOCK_INIT;
static double g_baseScale = 1.0;   // from frame_rate (1.0 = the game's 60)

typedef VOID(WINAPI* Sleep_t)(DWORD);
typedef DWORD(WINAPI* SleepEx_t)(DWORD, BOOL);
typedef DWORD(WINAPI* WFSO_t)(HANDLE, DWORD);
typedef DWORD(WINAPI* WFSOEx_t)(HANDLE, DWORD, BOOL);
typedef DWORD(WINAPI* WFMO_t)(DWORD, const HANDLE*, BOOL, DWORD);
typedef BOOL(WINAPI* QPC_t)(LARGE_INTEGER*);
typedef DWORD(WINAPI* TGT_t)();
typedef UINT(WINAPI* TBP_t)(UINT);
typedef BOOL(WINAPI* SetEvent_t)(HANDLE);
static Sleep_t oSleep;
static SleepEx_t oSleepEx;
static WFSO_t oWFSO;
static WFSOEx_t oWFSOEx;
static WFMO_t oWFMO;
static QPC_t oQPC;
static TGT_t oTGT;
static TBP_t oTBP;
static SetEvent_t oSetEvent;

struct Acc {
    DWORD tid = 0;
    unsigned sleeps = 0; double sleepMs = 0;
    std::map<DWORD, unsigned> sleepArgs;
    unsigned waits = 0; double waitMs = 0;
    std::map<DWORD, unsigned> waitTimeouts;
    unsigned qpc = 0, tgt = 0, setEvents = 0;
    unsigned longWaits = 0; double longWaitMs = 0;   // waits that blocked > 2 ms
    unsigned longSleeps = 0; double longSleepMs = 0;
    void Clear() { DWORD t = tid; *this = Acc(); tid = t; }
};
static std::vector<Acc*> g_all;
static SRWLOCK g_allLock = SRWLOCK_INIT;
static thread_local Acc* t_acc = nullptr;
static thread_local int t_inHook = 0;   // our own QPC calls inside hooks are not counted

static Acc* Me() {
    if (!t_acc) {
        t_acc = new Acc();
        t_acc->tid = GetCurrentThreadId();
        AcquireSRWLockExclusive(&g_allLock);
        g_all.push_back(t_acc);
        ReleaseSRWLockExclusive(&g_allLock);
    }
    return t_acc;
}

// frame timing (render thread); histogram buckets: <17.5, 17.5-20, 20-25, 25-30, 30-35, 35-50, >=50 ms
static const double kHist[] = {17.5, 20, 25, 30, 35, 50};
static unsigned g_hist[7] = {};
static unsigned g_frames = 0;
static double g_frameMs = 0, g_frameMaxMs = 0, g_presentMs = 0;
static LONGLONG g_lastPresentBegin = 0, g_presentBegin = 0;

static LONGLONG Now() { LARGE_INTEGER t; (oQPC ? oQPC : QueryPerformanceCounter)(&t); return t.QuadPart; }
static double Ms(LONGLONG a, LONGLONG b) { return (double)(b - a) * 1000.0 / (double)g_freq.QuadPart; }

struct HookScope { HookScope() { ++t_inHook; } ~HookScope() { --t_inHook; } };

static VOID WINAPI Hk_Sleep(DWORD ms) {
    HookScope h;
    LONGLONG t0 = Now();
    oSleep(ms);
    double d = Ms(t0, Now());
    Acc* a = Me(); a->sleeps++; a->sleepMs += d; a->sleepArgs[ms]++;
    if (d > 2) { a->longSleeps++; a->longSleepMs += d; }
}
static DWORD WINAPI Hk_SleepEx(DWORD ms, BOOL al) {
    HookScope h;
    LONGLONG t0 = Now();
    DWORD r = oSleepEx(ms, al);
    double d = Ms(t0, Now());
    Acc* a = Me(); a->sleeps++; a->sleepMs += d; a->sleepArgs[ms]++;
    if (d > 2) { a->longSleeps++; a->longSleepMs += d; }
    return r;
}
// Long waits on the render thread by calling module (game, d3d9 runtime, GPU driver, ...).
static std::map<std::string, std::pair<unsigned, double>> g_longWaitCallers;
static SRWLOCK g_lwLock = SRWLOCK_INIT;
static void NoteLongWait(void* ret, double ms) {
    if (GetCurrentThreadId() != g_renderTid) return;
    HMODULE m = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ret, &m)) {
        char p[MAX_PATH];
        GetModuleFileNameA(m, p, MAX_PATH);
        const char* b = strrchr(p, '\\');
        strcpy_s(name, b ? b + 1 : p);
        char off[24];
        sprintf_s(off, "+%lx", (unsigned long)((uintptr_t)ret - (uintptr_t)m));
        strcat_s(name, off);
    }
    AcquireSRWLockExclusive(&g_lwLock);
    auto& e = g_longWaitCallers[name];
    e.first++;
    e.second += ms;
    ReleaseSRWLockExclusive(&g_lwLock);
}

static DWORD WINAPI Hk_WFSO(HANDLE hd, DWORD ms) {
    HookScope h;
    LONGLONG t0 = Now();
    DWORD r = oWFSO(hd, ms);
    { double d = Ms(t0, Now()); if (d > 2) NoteLongWait(_ReturnAddress(), d); }
    double d = Ms(t0, Now());
    Acc* a = Me(); a->waits++; a->waitMs += d; a->waitTimeouts[ms]++;
    if (d > 2) { a->longWaits++; a->longWaitMs += d; }
    return r;
}
static DWORD WINAPI Hk_WFSOEx(HANDLE hd, DWORD ms, BOOL al) {
    HookScope h;
    LONGLONG t0 = Now();
    DWORD r = oWFSOEx(hd, ms, al);
    { double d = Ms(t0, Now()); if (d > 2) NoteLongWait(_ReturnAddress(), d); }
    double d = Ms(t0, Now());
    Acc* a = Me(); a->waits++; a->waitMs += d; a->waitTimeouts[ms]++;
    if (d > 2) { a->longWaits++; a->longWaitMs += d; }
    return r;
}
static DWORD WINAPI Hk_WFMO(DWORD n, const HANDLE* hs, BOOL all, DWORD ms) {
    HookScope h;
    LONGLONG t0 = Now();
    DWORD r = oWFMO(n, hs, all, ms);
    { double d = Ms(t0, Now()); if (d > 2) NoteLongWait(_ReturnAddress(), d); }
    double d = Ms(t0, Now());
    Acc* a = Me(); a->waits++; a->waitMs += d; a->waitTimeouts[ms]++;
    if (d > 2) { a->longWaits++; a->longWaitMs += d; }
    return r;
}
static BOOL WINAPI Hk_QPC(LARGE_INTEGER* t) {
    if (g_installed && !t_inHook) Me()->qpc++;
    BOOL r = oQPC(t);
    if (r && t && g_limiterTid && GetCurrentThreadId() == g_limiterTid) {
        AcquireSRWLockShared(&g_qpcLock);
        if (g_qpcRealBase) t->QuadPart = g_qpcScaledBase + (LONGLONG)((double)(t->QuadPart - g_qpcRealBase) * g_qpcScale);
        ReleaseSRWLockShared(&g_qpcLock);
    }
    return r;
}
static DWORD WINAPI Hk_TGT() {
    Me()->tgt++;
    return oTGT();
}
static BOOL WINAPI Hk_SetEvent(HANDLE h) {
    Me()->setEvents++;
    return oSetEvent(h);
}
typedef UINT(WINAPI* TSE_t)(UINT, UINT, LPTIMECALLBACK, DWORD_PTR, UINT);
typedef BOOL(WINAPI* SWT_t)(HANDLE, const LARGE_INTEGER*, LONG, PTIMERAPCROUTINE, LPVOID, BOOL);
typedef BOOL(WINAPI* CTQT_t)(PHANDLE, HANDLE, WAITORTIMERCALLBACK, PVOID, DWORD, DWORD, ULONG);
static TSE_t oTSE;
static SWT_t oSWT;
static CTQT_t oCTQT;
// The game's frame limiter (found with pacing_diag): one thread creates a 1 ms periodic multimedia timer, wakes on
// every tick, reads QueryPerformanceCounter and signals the main loop once 1/60 s has elapsed (one SetEvent per frame).
// frame_rate = N makes QueryPerformanceCounter run N/60 times faster *for that thread only*, so it releases N frames
// per second; every other thread (game logic, audio) keeps real time.
static UINT WINAPI Hk_TSE(UINT delay, UINT res, LPTIMECALLBACK cb, DWORD_PTR user, UINT flags) {
    UINT id = oTSE(delay, res, cb, user, flags);
    if (delay == 1 && (flags & TIME_PERIODIC) && !g_limiterTid) {
        g_limiterTid = GetCurrentThreadId();
        LOGI("pacing: frame limiter thread %lu (1 ms periodic timer)%s", g_limiterTid,
             g_qpcScale != 1.0 ? " - its clock now runs faster" : "");
    }
    LOGI("pacing: timeSetEvent(delay %u ms, resolution %u, flags %x) -> %u (thread %lu)", delay, res, flags, id, GetCurrentThreadId());
    return id;
}
static BOOL WINAPI Hk_SWT(HANDLE h, const LARGE_INTEGER* due, LONG period, PTIMERAPCROUTINE f, LPVOID a, BOOL r) {
    static int n = 0;
    if (n++ < 20) LOGI("pacing: SetWaitableTimer(due %lld x100ns, period %ld ms) thread %lu", due ? due->QuadPart : 0, period, GetCurrentThreadId());
    return oSWT(h, due, period, f, a, r);
}
static BOOL WINAPI Hk_CTQT(PHANDLE ph, HANDLE q, WAITORTIMERCALLBACK cb, PVOID p, DWORD due, DWORD period, ULONG flags) {
    LOGI("pacing: CreateTimerQueueTimer(due %lu ms, period %lu ms, flags %lx) thread %lu", due, period, flags, GetCurrentThreadId());
    return oCTQT(ph, q, cb, p, due, period, flags);
}
static UINT WINAPI Hk_TBP(UINT p) {
    LOGI("pacing: timeBeginPeriod(%u) from thread %lu", p, GetCurrentThreadId());
    return oTBP(p);
}

template <class T> static void Hook(HMODULE m, const char* name, void* fn, T* orig) {
    void* p = m ? (void*)GetProcAddress(m, name) : nullptr;
    if (!p) { LOGW("pacing: %s not found", name); return; }
    if (MH_CreateHook(p, fn, (void**)orig) == MH_OK && MH_EnableHook(p) == MH_OK) LOGD("pacing: hooked %s", name);
    else LOGW("pacing: could not hook %s", name);
}

// Sets the limiter thread's clock speed (1.0 = real time) without a jump in its time.
static void SetLimiterScale(double s) {
    if (s == g_qpcScale && g_qpcRealBase) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);   // real time: we are not on the limiter thread
    AcquireSRWLockExclusive(&g_qpcLock);
    LONGLONG scaledNow = g_qpcRealBase ? g_qpcScaledBase + (LONGLONG)((double)(now.QuadPart - g_qpcRealBase) * g_qpcScale) : now.QuadPart;
    g_qpcRealBase = now.QuadPart;
    g_qpcScaledBase = scaledNow;
    g_qpcScale = s;
    ReleaseSRWLockExclusive(&g_qpcLock);
}

void InstallPacingHooks() {
    static bool limiterHooked = false;
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    HMODULE winmm = GetModuleHandleW(L"winmm.dll");
    int fr = G().s.frameRate;
    bool sync = G().s.vrEnabled && G().s.headsetSync;
    if (!limiterHooked && winmm && ((fr > 0 && fr != 60) || G().s.pacingDiag || sync)) {
        limiterHooked = true;
        MH_Initialize();
        Hook(winmm, "timeSetEvent", (void*)&Hk_TSE, &oTSE);
        Hook(k32, "QueryPerformanceCounter", (void*)&Hk_QPC, &oQPC);
        if (fr > 0 && fr != 60) {
            g_baseScale = fr / 60.0;
            SetLimiterScale(g_baseScale);
            LOGI("pacing: frame_rate %d - limiter clock scale %.3f", fr, g_baseScale);
        }
    }
    if (g_installed || !G().s.pacingDiag) return;
    g_installed = true;
    QueryPerformanceFrequency(&g_freq);
    MH_Initialize();
    Hook(k32, "Sleep", (void*)&Hk_Sleep, &oSleep);
    Hook(k32, "SleepEx", (void*)&Hk_SleepEx, &oSleepEx);
    Hook(k32, "WaitForSingleObject", (void*)&Hk_WFSO, &oWFSO);
    Hook(k32, "WaitForSingleObjectEx", (void*)&Hk_WFSOEx, &oWFSOEx);
    Hook(k32, "WaitForMultipleObjects", (void*)&Hk_WFMO, &oWFMO);
    Hook(k32, "SetEvent", (void*)&Hk_SetEvent, &oSetEvent);
    Hook(k32, "SetWaitableTimer", (void*)&Hk_SWT, &oSWT);
    Hook(k32, "CreateTimerQueueTimer", (void*)&Hk_CTQT, &oCTQT);
    if (winmm) {
        Hook(winmm, "timeGetTime", (void*)&Hk_TGT, &oTGT);
        Hook(winmm, "timeBeginPeriod", (void*)&Hk_TBP, &oTBP);
    }
    LOGI("pacing diagnostics installed");
}

// ------------------------------------------------------------------ headset sync (phase lock)
// The game's own limiter runs on 1 ms timer ticks: frames come 16/17/18 ms apart, ~57.8 fps, on its own clock. The
// host shows the newest game frame at each headset frame, so the two clocks drift against each other: smooth while
// the phase is comfortable, bursts of stutter while it crosses a frame boundary (the beat).
// Phase lock: the game runs an even, exact 60 fps (its limiter is opened slightly to 62 and this wait sets the time),
// and each finished frame is handed to the host at a time locked to the headset's frames:
//     s_k = tick(B) + (pos_k + offset) * period,   pos_k = k * r,  r = headset rate / 60
// The pattern positions frac(k * r) form a lattice of spacing 1/q (90 Hz: r = 1.5, q = 2; 72 Hz: r = 1.2, q = 5;
// 120 Hz: q = 1); offset = 0.5/q puts every hand-over in the middle between two lattice points, i.e. as far as
// possible from a headset tick. The host therefore always picks the frames up in the same regular rhythm
// (90 Hz: 2,1,2,1...). tick() comes from an observer thread that timestamps every headset frame, so the schedule
// follows the real display clock and cannot drift. A late frame moves to the next lattice point.
static bool g_syncOn = false;
static int64_t g_syncPeriod = 0;
static double g_syncRatio = 0, g_syncOffset = 0, g_syncPos = 0;
static int g_syncQ = 1;
static uint32_t g_syncBase = 0;
static bool g_syncHaveBase = false;
static unsigned g_syncFrames = 0, g_syncLate = 0;
static double g_syncWaitMs = 0;
static unsigned g_syncSteps[4] = {};   // headset frames each game frame is shown for (1, 2, 3, 4+)
static LARGE_INTEGER g_qf{};

// Headset period as used by the schedule. SteamVR now and then reports a doubled period (22.22 ms at 90 Hz) for a
// single frame; restarting the schedule for that caused a hitch every 10-40 s (headset test m3d). A new period is
// only accepted once it has been reported for kPeriodSettleS.
static int64_t g_pAccepted = 0, g_pCandidate = 0;
static LONGLONG g_pCandSince = 0;
static unsigned g_syncBlips = 0;
static const double kPeriodSettleS = 0.5;

// latest observed headset tick (beat number + QPC time)
static SRWLOCK g_obsLock = SRWLOCK_INIT;
static uint32_t g_obsBeat = 0;
static LONGLONG g_obsQpc = 0;
static HANDLE g_obsThread = nullptr;
static volatile LONG g_obsSkips = 0;

static bool DebugClock() { return G().s.debugSyncPeriodMs > 0; }   // test aid: virtual headset clock at that period
static LONGLONG QpcNow() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }   // real time on this thread

// Beat numbers count display frames, from the host's predicted display time, so a host frame SteamVR skipped (or a
// doubled period) doesn't shift the schedule by a tick. The QPC time of a tick is filtered: the observer can only
// wake up late, never early, so a time earlier than predicted is taken at once and a later one only slowly.
static DWORD WINAPI TickObserver(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    int64_t lastXr = 0;   // kept across sync off/on, so beat numbers stay continuous for the life of the session
    for (;;) {
        if (DebugClock()) { Sleep(20); continue; }
        if (xrb::ClientWaitHostFrame(50)) {
            LONGLONG t = QpcNow();
            xrb::IpcPose pose[2];
            xrb::IpcFov fov[2];
            int64_t xr = 0;
            int64_t p = g_pAccepted;
            if (!xrb::ClientGetLatestPoses(pose, fov, &xr) || p <= 0) continue;
            double qpt = (double)g_qf.QuadPart * (double)p / 1e9;
            AcquireSRWLockExclusive(&g_obsLock);
            if (!lastXr || !g_obsQpc) {
                g_obsBeat++;
                g_obsQpc = t;
            } else if (xr != lastXr) {
                long d = (long)floor((double)(xr - lastXr) / (double)p + 0.5);
                if (d < 1) d = 1;
                if (d > 1) InterlockedAdd(&g_obsSkips, d - 1);
                g_obsBeat += (uint32_t)d;
                double pred = (double)g_obsQpc + d * qpt;
                double res = (double)t - pred;
                if (res < 0 || res > 4 * qpt) g_obsQpc = t;   // early: take it; far off: resync
                else g_obsQpc = (LONGLONG)(pred + res * 0.05);
            }
            lastXr = xr;
            ReleaseSRWLockExclusive(&g_obsLock);
        } else Sleep(5);
    }
}

// QPC time of (fractional) headset tick `beat`, from the latest observation.
static double TickTime(double beat, double qpcPerTick) {
    if (DebugClock()) return beat * qpcPerTick;   // virtual clock: tick n at n * period
    AcquireSRWLockShared(&g_obsLock);
    uint32_t b = g_obsBeat;
    LONGLONG q = g_obsQpc;
    ReleaseSRWLockShared(&g_obsLock);
    return (double)q + (beat - (double)b) * qpcPerTick;
}
static bool HaveTicks() {
    if (DebugClock()) return true;
    AcquireSRWLockShared(&g_obsLock);
    bool ok = g_obsQpc != 0;
    ReleaseSRWLockShared(&g_obsLock);
    return ok;
}
static uint32_t CurrentBeat(double qpcPerTick) {
    if (DebugClock()) return (uint32_t)((double)QpcNow() / qpcPerTick);
    AcquireSRWLockShared(&g_obsLock);
    uint32_t b = g_obsBeat;
    ReleaseSRWLockShared(&g_obsLock);
    return b;
}

bool PacingHeadsetSync(LONGLONG* handover) {
    // FF XIII / XIII-2: the 1 ms timer thread is opened up (SetLimiterScale) and the throttle sets the time. Lightning
    // Returns has no limiter thread: it busy-waits on QPC on the render thread itself, precisely, so only the throttle
    // is added (the game's own 1/60 s wait is already satisfied when the throttle releases a frame).
    if (!G().s.vrEnabled || !G().s.headsetSync || (!g_limiterTid && G().game != GAME_LR)) return false;
    if (!g_qf.QuadPart) QueryPerformanceFrequency(&g_qf);
    int64_t p = xrb::ClientDisplayPeriodNs();
    bool visible = xrb::ClientHostVisible();
    if (DebugClock()) { p = (int64_t)(G().s.debugSyncPeriodMs * 1e6); visible = true; }
    if (p > 0) {   // ignore period reports that don't last (see g_pAccepted)
        LONGLONG now = QpcNow();
        if (!g_pAccepted) g_pAccepted = p;
        else if (llabs(p - g_pAccepted) > g_pAccepted / 50) {
            if (!g_pCandidate || llabs(p - g_pCandidate) > g_pCandidate / 50) { g_pCandidate = p; g_pCandSince = now; }
            if ((double)(now - g_pCandSince) >= kPeriodSettleS * g_qf.QuadPart) {
                LOGI("headset sync: headset period changed %.2f -> %.2f ms", g_pAccepted / 1e6, p / 1e6);
                g_pAccepted = p;
                g_pCandidate = 0;
            }
        } else if (g_pCandidate) {
            g_syncBlips++;
            g_pCandidate = 0;
        }
        p = g_pAccepted;
    }
    double r = p > 0 ? 16666667.0 / (double)p : 0;
    // Below 60 Hz (a frame-generation layer such as OFXR 3X runs the host at 30) the same schedule hands every frame
    // over, and the host picks up every second one at an even spacing: only with sync_below_60.
    double rMin = G().s.syncBelow60 ? 0.24 : 0.99;
    if (!visible || r < rMin || r > 6.0) {
        if (g_syncOn) { g_syncOn = false; SetLimiterScale(g_baseScale); LOGI("headset sync off (headset period %.2f ms)", p / 1e6); }
        return false;
    }
    if (!g_obsThread) g_obsThread = CreateThread(nullptr, 0, TickObserver, nullptr, 0, nullptr);
    if (!g_syncOn || llabs(p - g_syncPeriod) > g_syncPeriod / 50) {   // (re)start, also when the headset rate changes
        g_syncOn = true;
        g_syncPeriod = p;
        g_syncRatio = r;
        double fr = r - floor(r);
        g_syncQ = 12;
        for (int q = 1; q <= 12; ++q)
            if (fabs(fr * q - floor(fr * q + 0.5)) < 0.02) { g_syncQ = q; break; }
        g_syncOffset = 0.5 / g_syncQ;
        g_syncHaveBase = false;
        SetLimiterScale(g_baseScale * 62.0 / 60.0);   // the game's limiter never holds a frame back; we set the time
        LOGI("headset sync on: headset %.2f Hz, %.3f headset frames per game frame, hand-over %.2f ms from the nearest tick",
             1e9 / p, r, g_syncOffset * p / 1e6);
    }
    if (!HaveTicks()) return false;   // observer has not seen a headset frame yet
    double qpt = (double)g_qf.QuadPart * (double)p / 1e9;   // QPC units per headset tick
    bool first = !g_syncHaveBase;
    if (first) {
        g_syncBase = CurrentBeat(qpt) + 1;
        g_syncPos = 0;
        g_syncHaveBase = true;
    } else {
        g_syncPos += g_syncRatio;
    }
    double oldPos = g_syncPos - g_syncRatio;
    LONGLONG now = QpcNow();
    double margin = (double)g_qf.QuadPart * 0.001;   // the GPU still has to finish the copy for VR (~1 ms)
    double s = TickTime(g_syncBase + g_syncPos + g_syncOffset, qpt);
    if ((double)now + margin > s) {   // late: move to the next lattice point (this frame is shown longer, unavoidably)
        g_syncLate++;
        double x = ((double)now + margin - TickTime(g_syncBase, qpt)) / qpt - g_syncOffset;
        g_syncPos = ceil(x * g_syncQ - 1e-6) / g_syncQ;
        if (g_syncPos < oldPos + 1.0 / g_syncQ) g_syncPos = oldPos + 1.0 / g_syncQ;
        s = TickTime(g_syncBase + g_syncPos + g_syncOffset, qpt);
    }
    *handover = (LONGLONG)s;
    // statistics: for how many headset frames the previous game frame stays on screen
    if (!first) {
        int shown = (int)floor(g_syncPos + g_syncOffset) - (int)floor(oldPos + g_syncOffset);
        g_syncSteps[shown >= 4 ? 3 : (shown < 1 ? 0 : shown - 1)]++;
    }
    if (g_syncPos > 100000) {   // keep the numbers small
        double whole = floor(g_syncPos);
        g_syncBase += (uint32_t)whole;
        g_syncPos -= whole;
    }
    g_syncFrames++;
    return true;
}

// The game may run at most one frame ahead of the hand-over schedule: Present of frame k returns at s_k - 1/60 s at the
// earliest. That fixes the game at 60 frames per second and leaves each frame up to two frame periods to finish.
void PacingSyncThrottle(LONGLONG handover) {
    if (!g_qf.QuadPart) QueryPerformanceFrequency(&g_qf);
    double until = (double)handover - (double)g_qf.QuadPart / 60.0;
    LONGLONG t0 = QpcNow();
    for (;;) {
        double remainMs = (until - (double)QpcNow()) * 1000.0 / g_qf.QuadPart;
        if (remainMs <= 0) break;
        if (remainMs > 1.5) Sleep(1); else YieldProcessor();   // the game runs timeBeginPeriod(1)
    }
    g_syncWaitMs += (double)(QpcNow() - t0) * 1000.0 / g_qf.QuadPart;
}

void PacingSyncLogStatus() {
    if (!g_syncFrames) return;
    LOGI("headset sync: %u frames, %u late, avg wait %.2f ms; frames shown for 1/2/3/4+ headset frames: %u/%u/%u/%u; "
         "ignored period blips %u, headset frames skipped by the runtime %ld", g_syncFrames, g_syncLate,
         g_syncWaitMs / g_syncFrames, g_syncSteps[0], g_syncSteps[1], g_syncSteps[2], g_syncSteps[3], g_syncBlips,
         InterlockedExchange(&g_obsSkips, 0));
    g_syncFrames = g_syncLate = g_syncBlips = 0;
    g_syncWaitMs = 0;
    memset(g_syncSteps, 0, sizeof(g_syncSteps));
}

void PacingPresentBegin() {
    if (!g_installed) return;
    if (!g_renderTid) { g_renderTid = GetCurrentThreadId(); LOGI("pacing: render thread %lu", g_renderTid); }
    LONGLONG t = Now();
    if (g_lastPresentBegin) {
        double f = Ms(g_lastPresentBegin, t);
        g_frames++; g_frameMs += f;
        int b = 0;
        while (b < 6 && f >= kHist[b]) b++;
        g_hist[b]++;
        if (f > g_frameMaxMs) g_frameMaxMs = f;
    }
    g_lastPresentBegin = t;
    g_presentBegin = t;
}

void PacingPresentEnd() {
    if (!g_installed) return;
    g_presentMs += Ms(g_presentBegin, Now());
}

static std::string Describe(const Acc& a, double n) {
    std::string sl, wt;
    char b[96];
    int k = 0;
    for (auto& kv : a.sleepArgs) { if (k++ < 6) { sprintf_s(b, "%lu:%.1f ", kv.first, kv.second / n); sl += b; } }
    k = 0;
    for (auto& kv : a.waitTimeouts) { if (k++ < 6) { sprintf_s(b, "%ld:%.1f ", (long)kv.first, kv.second / n); wt += b; } }
    sprintf_s(b, "thread %5lu%s: ", a.tid, a.tid == g_renderTid ? " (render)" : "");
    std::string s = b;
    char c[400];
    sprintf_s(c, "sleep %.1f/fr %.2f ms [%s] (>2ms: %.2f/fr %.2f ms) wait %.1f/fr %.2f ms [%s] (>2ms: %.2f/fr %.2f ms) QPC %.0f/fr timeGetTime %.0f/fr SetEvent %.1f/fr",
              a.sleeps / n, a.sleepMs / n, sl.c_str(), a.longSleeps / n, a.longSleepMs / n, a.waits / n, a.waitMs / n, wt.c_str(), a.longWaits / n,
              a.longWaitMs / n, a.qpc / n, a.tgt / n, a.setEvents / n);
    return s + c;
}

void PacingLogStatus() {
    if (!g_installed || !g_frames) return;
    double n = g_frames;
    LOGI("pacing: frame %.2f ms avg, %.2f max; in Present %.2f ms (per-frame counts below; sleep/wait args are ms:count)",
         g_frameMs / n, g_frameMaxMs, g_presentMs / n);
    LOGI("pacing: frame times <17.5 ms %u, 17.5-20 %u, 20-25 %u, 25-30 %u, 30-35 %u, 35-50 %u, >=50 %u", g_hist[0], g_hist[1], g_hist[2],
         g_hist[3], g_hist[4], g_hist[5], g_hist[6]);
    memset(g_hist, 0, sizeof(g_hist));
    AcquireSRWLockShared(&g_allLock);
    std::vector<Acc*> v = g_all;
    ReleaseSRWLockShared(&g_allLock);
    // render thread first, then the threads with the most timing activity
    std::sort(v.begin(), v.end(), [](Acc* a, Acc* b) {
        if ((a->tid == g_renderTid) != (b->tid == g_renderTid)) return a->tid == g_renderTid;
        return a->qpc + a->tgt + a->setEvents * 10 + a->sleeps > b->qpc + b->tgt + b->setEvents * 10 + b->sleeps;   // timing activity
    });
    int shown = 0;
    for (Acc* a : v) {
        if (shown++ >= 8) break;
        if (!a->sleeps && !a->waits && !a->qpc && !a->tgt) continue;
        LOGI("pacing:   %s", Describe(*a, n).c_str());
    }
    for (Acc* a : v) a->Clear();   // racy with the owning thread but fine for diagnostics
    AcquireSRWLockExclusive(&g_lwLock);
    for (auto& kv : g_longWaitCallers)
        LOGI("pacing:   render thread long waits from %s: %.2f/frame, %.2f ms/frame", kv.first.c_str(), kv.second.first / n, kv.second.second / n);
    g_longWaitCallers.clear();
    ReleaseSRWLockExclusive(&g_lwLock);
    g_frames = 0; g_frameMs = g_frameMaxMs = g_presentMs = 0;
}

} // namespace fvr
