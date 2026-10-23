#pragma once
#include "core/compare.hpp"
#include <functional>
#include <memory>
#include <mutex>
#include <set>
namespace hc::compare
{
    // Admission uses private commit, not working set. Controller checks are
    // cooperative; child reservations are enforced by Windows Job Objects.
    class ControllerMemory final : public std::enable_shared_from_this<ControllerMemory>
    {
    public:
        explicit ControllerMemory(std::function<std::uint64_t()> Measure);
        struct WorkerReservation { std::uint64_t bytes_ = 0; std::shared_ptr<void> lease_; };
        std::shared_ptr<void> Enter(std::uint64_t Limit);
        void Check();
        WorkerReservation TryWorker(std::uint64_t Maximum, std::uint64_t Minimum, std::uint64_t Overhead);
        std::uint64_t Reserved() const;
    private:
        struct RequestLease;
        struct WorkerLease;
        void checkLocked(bool Fresh);
        std::uint64_t ceiling() const;
        std::function<std::uint64_t()> measure_;
        mutable std::mutex mutex_;
        std::multiset<std::uint64_t> limits_;
        std::uint64_t reserved_ = 0, workerBytes_ = 0, measured_ = 0;
        std::chrono::steady_clock::time_point sampled_{};
    };
    std::shared_ptr<ControllerMemory> ProcessControllerMemory();
    void ConfigureMemoryBudget(Budget& Limits, int MemoryMiB);
}
