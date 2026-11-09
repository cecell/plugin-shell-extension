#include "work_budget.h"

#include <windows.h>

#include <sddl.h>

#include <algorithm>
#include <mutex>

#include "disk_cache.h"

namespace pse {
namespace {

constexpr int kSlots = 64;

struct Shared {
    volatile LONG owners[kSlots];  // process IDs; 0 = free
};

// The DLL also loads into low-integrity preview hosts, so the shared block has
// to be open to them. If it can't be created, this process budgets alone.
Shared* Block() {
    static Shared* block = [] {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, FALSE};
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;WD)S:(ML;;NW;;;LW)", SDDL_REVISION_1, &sd, nullptr))
            sa.lpSecurityDescriptor = sd;
        HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(Shared), L"Local\\PluginShellExtensionWorkBudget");
        if (!map) map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, L"Local\\PluginShellExtensionWorkBudget");
        if (sd) LocalFree(sd);
        void* view = map ? MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)) : nullptr;
        static Shared local{};
        return view ? static_cast<Shared*>(view) : &local;
    }();
    return block;
}

bool ProcessAlive(DWORD pid) {
    if (pid == GetCurrentProcessId()) return true;
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return GetLastError() != ERROR_INVALID_PARAMETER;  // access denied still means it exists
    const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
}

// A process that crashed while holding a slot must not keep it forever.
void ReclaimDead(Shared* s) {
    for (int i = 0; i < kSlots; ++i) {
        const LONG pid = s->owners[i];
        if (pid && !ProcessAlive(static_cast<DWORD>(pid))) InterlockedCompareExchange(&s->owners[i], 0, pid);
    }
}

int CountTaken(Shared* s) {
    int n = 0;
    for (int i = 0; i < kSlots; ++i) n += s->owners[i] != 0;
    return n;
}

uint64_t FileTimeValue(const FILETIME& t) { return (static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime; }

// System-wide CPU use, sampled at most four times a second.
int CpuPercent() {
    static std::mutex lock;
    static uint64_t lastIdle = 0, lastTotal = 0, lastTick = 0;
    static int percent = 0;
    std::lock_guard<std::mutex> g(lock);
    const uint64_t now = GetTickCount64();
    if (now - lastTick < 250) return percent;
    FILETIME idle, kernel, user;
    if (!GetSystemTimes(&idle, &kernel, &user)) return percent;
    const uint64_t i = FileTimeValue(idle), total = FileTimeValue(kernel) + FileTimeValue(user);
    if (lastTick && total > lastTotal) percent = static_cast<int>(100 - (i - lastIdle) * 100 / (total - lastTotal));
    lastIdle = i;
    lastTotal = total;
    lastTick = now;
    return percent;
}

}  // namespace

int WorkerLimit() { return std::clamp(WorkerThreadCount(), 1, kSlots); }

int ActiveWorkers() { return CountTaken(Block()); }

unsigned PaceDelayMs(int cpuPercent, int active, int limit) {
    const double cpu = std::clamp(cpuPercent, 0, 100);
    const double pressure = cpu <= 50 ? 0 : (cpu - 50) / 50;
    const double fromCpu = pressure * pressure * 200;
    const double crowd = limit > 0 && active > 1 ? static_cast<double>(active - 1) * active / limit : 0;
    return static_cast<unsigned>(std::min(400.0, fromCpu + crowd * 10));
}

WorkSlot::WorkSlot(WorkKind kind, const std::atomic<bool>* cancel) : kind_(kind) {
    Shared* s = Block();
    const LONG me = static_cast<LONG>(GetCurrentProcessId());
    uint64_t lastClean = 0;
    while (!(cancel && cancel->load())) {
        const uint64_t now = GetTickCount64();
        if (now - lastClean >= 2000) {
            ReclaimDead(s);
            lastClean = now;
        }
        const int limit = WorkerLimit();
        if (CountTaken(s) < limit) {
            for (int i = 0; i < kSlots && index_ < 0; ++i)
                if (InterlockedCompareExchange(&s->owners[i], me, 0) == 0) index_ = i;
            // Two threads can pass the count together: the one over the limit steps back.
            if (index_ >= 0 && CountTaken(s) > limit) {
                InterlockedExchange(&s->owners[index_], 0);
                index_ = -1;
            }
        }
        if (index_ >= 0) break;
        Sleep(40);
    }
    if (index_ < 0) return;
    priority_ = GetThreadPriority(GetCurrentThread());
    if (kind_ == WorkKind::Background) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    else SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    lastRest_ = GetTickCount64();
}

WorkSlot::~WorkSlot() {
    if (index_ < 0) return;
    if (kind_ == WorkKind::Background) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    else SetThreadPriority(GetCurrentThread(), priority_);
    InterlockedExchange(&Block()->owners[index_], 0);
}

void WorkSlot::Pace() {
    if (index_ < 0 || GetTickCount64() - lastRest_ < 25) return;
    const unsigned delay = PaceDelayMs(CpuPercent(), ActiveWorkers(), WorkerLimit());
    if (delay) Sleep(delay);
    lastRest_ = GetTickCount64();
}

}  // namespace pse
