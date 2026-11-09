// One budget for background work across the whole suite. Anything long-running
// (ITM checks, extracting, testing, hashing, and anything lazy-loaded later)
// takes a WorkSlot first, so the total stays under one limit however many
// Explorer, preview and indexer processes have the DLL loaded.
//
// The limit is "workerThreads" in settings.json: 0 = half the physical cores,
// from 1 to 6. Work also slows itself as the machine gets busier: call
// WorkSlot::Pace() between pieces of work.
#pragma once

#include <atomic>
#include <cstdint>

namespace pse {

enum class WorkKind {
    Background,   // nobody is waiting: lowest CPU, disk and memory priority
    Interactive,  // the user asked and is waiting: below normal CPU priority
};

class WorkSlot {
public:
    // Waits for a free slot. Held() is false if `cancel` was set first.
    explicit WorkSlot(WorkKind kind, const std::atomic<bool>* cancel = nullptr);
    ~WorkSlot();
    WorkSlot(const WorkSlot&) = delete;
    WorkSlot& operator=(const WorkSlot&) = delete;

    bool Held() const { return index_ >= 0; }

    // Rests for a moment if the machine is busy. Cheap to call often.
    void Pace();

private:
    int index_ = -1;
    WorkKind kind_;
    int priority_ = 0;
    uint64_t lastRest_ = 0;
};

// How many slots there can be, and how many are taken right now, across processes.
int WorkerLimit();
int ActiveWorkers();

// How long to rest (ms) at this system CPU percentage and this many workers.
// It rises with both, and is what makes work back off as the UI comes under load.
unsigned PaceDelayMs(int cpuPercent, int active, int limit);

}  // namespace pse
