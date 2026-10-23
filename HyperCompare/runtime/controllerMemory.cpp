#include "stdafx.h"
#include "controllerMemory.hpp"
#include <Windows.h>
#include <psapi.h>
namespace hc::compare
{
    namespace { constexpr std::uint64_t mib = 1024 * 1024; }
    struct ControllerMemory::RequestLease
    {
        std::shared_ptr<ControllerMemory> owner_; std::uint64_t limit_ = 0; bool active_ = false;
        ~RequestLease() { if (active_) { std::lock_guard lock(owner_->mutex_); owner_->limits_.erase(owner_->limits_.find(limit_)); } }
    };
    struct ControllerMemory::WorkerLease
    {
        std::shared_ptr<ControllerMemory> owner_; std::uint64_t bytes_ = 0, child_ = 0;
        ~WorkerLease() { if (bytes_) { std::lock_guard lock(owner_->mutex_); owner_->reserved_ -= bytes_; owner_->workerBytes_ -= child_; } }
    };
    ControllerMemory::ControllerMemory(std::function<std::uint64_t()> Measure) : measure_(std::move(Measure)) {}
    std::uint64_t ControllerMemory::ceiling() const { return limits_.empty() ? 2048 * mib : *limits_.begin(); }
    void ControllerMemory::checkLocked(bool Fresh)
    {
        const auto now = std::chrono::steady_clock::now();
        if (Fresh || now - sampled_ >= std::chrono::milliseconds(20)) { measured_ = measure_(); sampled_ = now; }
        const auto limit = ceiling();
        if (measured_ > limit || workerBytes_ > limit - measured_) throw Failure("ControllerMemoryLimit");
    }
    std::shared_ptr<void> ControllerMemory::Enter(std::uint64_t Limit)
    {
        if (Limit < 256 * mib || Limit > 2048 * mib) throw Failure("InvalidMemoryLimit");
        auto lease = std::make_shared<RequestLease>(); lease->owner_ = shared_from_this(); lease->limit_ = Limit;
        // The lease is destroyed after this lock on failure, removing its limit.
        std::lock_guard lock(mutex_); limits_.insert(Limit); lease->active_ = true; checkLocked(true); return lease;
    }
    void ControllerMemory::Check() { std::lock_guard lock(mutex_); checkLocked(false); }
    ControllerMemory::WorkerReservation ControllerMemory::TryWorker(std::uint64_t Maximum, std::uint64_t Minimum, std::uint64_t Overhead)
    {
        if (!Minimum || Maximum < Minimum || Maximum > 512 * mib || Overhead > 512 * mib) throw Failure("WorkerPolicy");
        auto lease = std::make_shared<WorkerLease>(); lease->owner_ = shared_from_this();
        std::lock_guard lock(mutex_); checkLocked(true);
        // Headroom is reserved for admission, but once allocated it is already
        // in measured private commit and must not be counted twice by Check.
        if (reserved_ > ceiling() - measured_) return {};
        const auto available = ceiling() - measured_ - reserved_;
        if (Overhead > available || Minimum > available - Overhead) return {};
        const auto bytes = std::min(Maximum, available - Overhead);
        lease->bytes_ = bytes + Overhead; lease->child_ = bytes; reserved_ += lease->bytes_; workerBytes_ += bytes; return {bytes, lease};
    }
    std::uint64_t ControllerMemory::Reserved() const { std::lock_guard lock(mutex_); return reserved_; }
    std::shared_ptr<ControllerMemory> ProcessControllerMemory()
    {
        static const auto pool = std::make_shared<ControllerMemory>([]() -> std::uint64_t {
            PROCESS_MEMORY_COUNTERS_EX counters{}; counters.cb = sizeof(counters);
            if (!K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) throw Failure("MemoryMeasurementUnavailable");
            return counters.PrivateUsage;
        });
        return pool;
    }
    void ConfigureMemoryBudget(Budget& Limits, int MemoryMiB)
    {
        if (MemoryMiB < 256 || MemoryMiB > 2048) throw Failure("InvalidMemoryLimit");
        const auto pool = ProcessControllerMemory(); const auto lease = pool->Enter(std::uint64_t(MemoryMiB) * mib);
        Limits.memoryCheck_ = [pool, lease] { pool->Check(); };
        Limits.Check();
    }
}
