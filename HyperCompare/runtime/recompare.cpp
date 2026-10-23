#include "stdafx.h"
#include "session.hpp"
#include "controllerMemory.hpp"
#include <QCryptographicHash>
#include <QHash>

namespace hc::compare
{
    namespace
    {
        QByteArray signature(const Result& Session, const MergeBlock& Block)
        {
            QCryptographicHash hash(QCryptographicHash::Sha256);
            const auto add = [&](QByteArrayView Bytes) {
                hash.addData(QByteArray::number(Bytes.size()) + ':'); hash.addData(Bytes);
            };
            const auto side = [&](int Index, std::size_t Begin, const std::string& Chunk) {
                const auto& text = Session.inputs_.at(Index).utf8_;
                if (Begin > std::size_t(text.size()) || Chunk.size() > std::size_t(text.size()) - Begin ||
                    std::string_view(text.constData() + Begin, Chunk.size()) != Chunk) throw Failure("InvalidMergeLocation");
                const auto before = std::min<std::size_t>(64, Begin);
                const auto after = std::min<std::size_t>(64, std::size_t(text.size()) - Begin - Chunk.size());
                add(QByteArrayView(text.constData() + Begin - before, qsizetype(before)));
                add(QByteArrayView(Chunk.data(), qsizetype(Chunk.size())));
                add(QByteArrayView(text.constData() + Begin + Chunk.size(), qsizetype(after)));
            };
            side(0, Block.leftBegin_, Block.left_); side(1, Block.rightBegin_, Block.right_);
            if (Session.plan_->hasBase_) side(2, Block.baseBegin_, Block.base_);
            return hash.result();
        }
    }
    Result Recompare(const Request& Input, const Result& Previous, const std::vector<BlockState>& State,
        std::size_t ResultBytes, const std::shared_ptr<std::atomic_bool>& Canceled)
    {
        Budget limits; limits.canceled_ = Canceled.get();
        limits.deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(Input.timeoutMs_);
        // This permission is only for inspection: Execute never writes here, and
        // the destination expectation captured before recompare is retained below.
        auto request = Input; request.overwrite_ = true;
        auto result = Execute(request, Canceled, false);
        if (result.completion_ != "Complete") return result;
        try
        {
            ConfigureMemoryBudget(limits, Input.memoryMiB_);
            if (!Previous.plan_ || !result.plan_ || Previous.plan_->hasBase_ != result.plan_->hasBase_ ||
                State.size() != Previous.plan_->blocks_.size() || Previous.inputs_.size() != result.inputs_.size() ||
                !Previous.hasOutputExpectation_) throw Failure("InvalidRecompareSession");
            result.expected_ = Previous.expected_; result.hasOutputExpectation_ = true;
            for (qsizetype i = 0; i < result.inputs_.size(); ++i)
            {
                const auto& before = Previous.inputs_[i]; const auto& after = result.inputs_[i];
                if (before.path_ != after.path_) throw Failure("InvalidRecompareSession");
                result.inputsChanged_ |= before.raw_ != after.raw_ || before.encoding_ != after.encoding_;
            }
            limits.Check();
            if (!result.inputsChanged_) { result.plan_ = Previous.plan_; return result; }
            QHash<QByteArray, qsizetype> old; QHash<QByteArray, int> counts;
            for (std::size_t i = 0; i < Previous.plan_->blocks_.size(); ++i)
            {
                limits.Check(); const auto key = signature(Previous, Previous.plan_->blocks_[i]);
                if (old.contains(key)) old[key] = -1; else old.insert(key, qsizetype(i));
            }
            QVector<QByteArray> keys; keys.reserve(qsizetype(result.plan_->blocks_.size()));
            for (const auto& block : result.plan_->blocks_)
            { limits.Check(); auto key = signature(result, block); ++counts[key]; keys.push_back(std::move(key)); }
            auto plan = std::make_shared<MergePlan>(*result.plan_);
            std::size_t previousEnd = 0; qsizetype previousIndex = -1; bool ordered = true;
            for (std::size_t i = 0; i < plan->blocks_.size(); ++i)
            {
                limits.Check(); auto& target = plan->initial_[i]; target = {0, 0, Resolution::Unresolved, false};
                const auto index = old.value(keys[qsizetype(i)], -1);
                if (index < 0 || counts.value(keys[qsizetype(i)]) != 1) continue;
                const auto& candidate = State[std::size_t(index)];
                if (!candidate.mapped_ || candidate.begin_ > candidate.end_ || candidate.end_ > ResultBytes) continue;
                target = candidate; ordered &= previousIndex < index && previousEnd <= target.begin_;
                previousEnd = target.end_; previousIndex = index;
            }
            // Moved/reordered blocks must not reuse ranges in a different RESULT order.
            if (!ordered) for (auto& state : plan->initial_) state = {0, 0, Resolution::Unresolved, false};
            result.plan_ = std::move(plan);
            limits.Check();
        }
        catch (const std::exception& error)
        {
            result.error_ = QString::fromUtf8(error.what()); result.exitCode_ = result.error_ == "Canceled" ? 3 : 2;
            result.completion_ = result.exitCode_ == 3 ? "Canceled" : "Failed";
        }
        return result;
    }
}
