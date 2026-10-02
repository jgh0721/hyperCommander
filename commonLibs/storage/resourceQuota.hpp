#pragma once
#include "storage.hpp"
#include <array>

namespace hc::storage
{
    enum class ResourceKind : std::size_t { Memory, Buffer, Session, Stream, Staging, Count };
    // Counts concurrent reservations, never cumulative bytes processed. A lease
    // owns its accounting state, so destruction of a provider cannot leak quota.
    class ResourceQuota final
    {
        static constexpr auto Count = static_cast<std::size_t>(ResourceKind::Count);
        struct State
        {
            std::mutex mutex_;
            std::array<std::uint64_t, Count> limits_, used_{};
            explicit State(std::array<std::uint64_t, Count> Limits) : limits_(Limits) {}
        };
    public:
        class Lease final
        {
        public:
            Lease() = default;
            Lease(const Lease&) = delete;
            Lease& operator=(const Lease&) = delete;
            Lease(Lease&& Other) noexcept : state_(std::move(Other.state_)), kind_(Other.kind_), amount_(std::exchange(Other.amount_, 0)) {}
            Lease& operator=(Lease&& Other) noexcept
            {
                if (this != &Other) { Reset(); state_ = std::move(Other.state_); kind_ = Other.kind_; amount_ = std::exchange(Other.amount_, 0); }
                return *this;
            }
            ~Lease() { Reset(); }
            void Reset()
            {
                if (state_) { std::lock_guard Lock(state_->mutex_); state_->used_[kind_] -= amount_; }
                amount_ = 0; state_.reset();
            }
        private:
            friend class ResourceQuota;
            Lease(std::shared_ptr<State> Value, std::size_t Kind, std::uint64_t Amount) : state_(std::move(Value)), kind_(Kind), amount_(Amount) {}
            std::shared_ptr<State> state_;
            std::size_t kind_ = 0;
            std::uint64_t amount_ = 0;
        };
        explicit ResourceQuota(std::array<std::uint64_t, Count> Limits) : state_(std::make_shared<State>(Limits)) {}
        Result<Lease> Acquire(ResourceKind Kind, std::uint64_t Amount, const OperationContext& Context = {})
        {
            if (const auto Stop = Context.Check({"resource-quota"})) return *Stop;
            const auto Index = static_cast<std::size_t>(Kind);
            if (Index >= Count) return Error{ErrorCategory::InvalidRequest, "resource-kind"};
            std::lock_guard Lock(state_->mutex_);
            if (Amount > state_->limits_[Index]) return Error{ErrorCategory::BudgetExceeded, "resource-reservation-too-large", {}, {}, false, "change-resource-budget-or-storage"};
            if (Amount > state_->limits_[Index] - state_->used_[Index])
                return Error{ErrorCategory::BudgetExceeded, "resource-busy", {}, {}, true, "release-resources-or-reschedule"};
            state_->used_[Index] += Amount;
            return Lease(state_, Index, Amount);
        }
        std::uint64_t Used(ResourceKind Kind) const
        {
            std::lock_guard Lock(state_->mutex_); const auto Index = static_cast<std::size_t>(Kind);
            return Index < Count ? state_->used_[Index] : 0;
        }
    private:
        std::shared_ptr<State> state_;
    };
}
