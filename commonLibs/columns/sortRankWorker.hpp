#pragma once
#include "columnSort.hpp"
#include <QCollatorSortKey>
#include <QCoreApplication>
#include <QThreadPool>
#include <algorithm>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>

// 여러 위치 결과(Everything · 평면 보기)의 열 정렬 순위를 작업 스레드에서 계산한다.
// UI 스레드는 묶음의 정렬 값(SortRow)만 넘기고, 순위 배열을 받아 바꾼다.
// 문자열 값은 작업 스레드에서 QCollatorSortKey로 바꿔 두므로 병합 비교가 정렬 키 비교로 끝난다.
// 비교 규칙은 SortLess와 같다(상위 폴더 → 폴더 → 열 값 → 식별자).
namespace hc::columns
{
    struct SortTextOptions
    {
        bool locale_ = true, numeric_ = true;
        Qt::CaseSensitivity case_ = Qt::CaseInsensitive;
        bool operator==(const SortTextOptions&) const = default;
    };

    class SortRankWorker
    {
    public:
        // Rows: 순위가 매겨진 앞쪽 행 수. Ranks가 비면 작업 스레드 상태가 어긋났으므로 전체를 다시 보내야 한다.
        using Ready = std::function<void(quint64 Generation, int Rows, QVector<int> Ranks)>;
        explicit SortRankWorker(Ready Callback) : state_(std::make_shared<State>()) { state_->ready_ = std::move(Callback); }
        ~SortRankWorker() { Clear(std::numeric_limits<quint64>::max()); std::scoped_lock Lock(state_->mutex_); state_->ready_ = {}; }
        SortRankWorker(const SortRankWorker&) = delete;
        SortRankWorker& operator=(const SortRankWorker&) = delete;

        // Reset이면 이전 행을 버리고 First는 0이어야 한다. 아니면 First는 앞서 보낸 행 수와 같아야 한다.
        void Submit(quint64 Generation, bool Reset, QVector<Qt::SortOrder> Orders, SortTextOptions Options, int First, QVector<SortRow> Rows)
        {
            Job Item{Generation, Reset, std::move(Orders), Options, First, std::move(Rows)};
            std::scoped_lock Lock(state_->mutex_);
            // 새 세대가 시작되면 아직 처리하지 않은 이전 세대의 작업은 의미가 없다.
            if (Reset) state_->queue_.clear();
            state_->latest_ = std::max(state_->latest_, Generation);
            state_->queue_.push_back(std::move(Item));
            if (!state_->running_) { state_->running_ = true; start(state_); }
        }
        // 검색 · 평면 보기가 끝나면 정렬 키를 해제한다. Generation보다 앞선 세대의 결과는 더 알리지 않는다.
        void Clear(quint64 Generation)
        {
            std::scoped_lock Lock(state_->mutex_);
            state_->queue_.clear(); state_->latest_ = std::max(state_->latest_, Generation);
            if (!state_->running_) state_->Release(); else state_->releasePending_ = true;
        }
        // 진단 · 시험용: 처리 대기 중인 작업이 없고 스레드가 쉬는 중인지.
        bool Idle() const { std::scoped_lock Lock(state_->mutex_); return !state_->running_ && state_->queue_.empty(); }

    private:
        struct KeyedValue
        {
            bool ready_ = false, text_ = false;
            QString string_;
            std::optional<QCollatorSortKey> key_;
            QVariant raw_;
        };
        struct KeyedRow
        {
            QString id_;
            bool parent_ = false, directory_ = false;
            std::vector<KeyedValue> values_;
            // 트리 순서: 구간마다 (파일 여부, 정렬 키). 같은 부모 안에서 폴더가 먼저, 부모가 자식보다 먼저다.
            std::vector<std::pair<bool, KeyedValue>> tree_;
        };
        struct Job
        {
            quint64 generation_ = 0;
            bool reset_ = false;
            QVector<Qt::SortOrder> orders_;
            SortTextOptions options_;
            int first_ = 0;
            QVector<SortRow> rows_;
        };
        struct State
        {
            mutable std::mutex mutex_;
            std::deque<Job> queue_;
            Ready ready_;
            quint64 latest_ = 0;
            bool running_ = false, releasePending_ = false;
            // 작업 스레드만 아래 상태를 쓴다(한 번에 하나의 실행만 있다).
            quint64 generation_ = 0;
            QVector<Qt::SortOrder> orders_;
            SortTextOptions options_;
            std::optional<QCollator> collator_;
            std::function<int(const QString&, const QString&)> plain_;
            std::vector<KeyedRow> rows_;
            std::vector<int> order_;
            void Release() { rows_ = {}; order_ = {}; collator_.reset(); }
        };
        static QThreadPool& pool()
        {
            static QThreadPool Pool; static const bool Initialized = [] { Pool.setMaxThreadCount(2); Pool.setExpiryTimeout(30000); return true; }();
            Q_UNUSED(Initialized); return Pool;
        }
        static void start(const std::shared_ptr<State>& Shared) { pool().start([Shared] { run(Shared); }); }
        static KeyedRow keyed(State& S, SortRow Row)
        {
            KeyedRow Result{std::move(Row.id_), Row.parent_, Row.directory_, {}};
            for (qsizetype Index = 0; Index < Row.tree_.size(); ++Index) {
                KeyedValue Segment; Segment.ready_ = Segment.text_ = true; Segment.string_ = Row.tree_[Index];
                if (S.options_.locale_) Segment.key_ = S.collator_->sortKey(Segment.string_);
                Result.tree_.push_back({Index + 1 == Row.tree_.size() && !Result.directory_, std::move(Segment)});
            }
            Result.values_.reserve(Row.values_.size());
            for (const auto& Value : Row.values_) {
                KeyedValue Item; Item.ready_ = Value.State() == ValueState::Ready;
                if (Item.ready_) {
                    Item.text_ = Value.Type() == FieldType::Text;
                    if (Item.text_) {
                        Item.string_ = std::get<QString>(*Value.Value());
                        if (S.options_.locale_) Item.key_ = S.collator_->sortKey(Item.string_);
                    } else Item.raw_ = Value.Raw();
                }
                Result.values_.push_back(std::move(Item));
            }
            return Result;
        }
        static bool less(const State& S, const KeyedRow& Left, const KeyedRow& Right)
        {
            if (Left.parent_ != Right.parent_) return Left.parent_;
            if (!Left.tree_.empty() && !Right.tree_.empty()) {
                // 이름 방향(첫 정렬 기준)은 같은 부모 안의 순서에만 적용한다. 폴더 먼저 · 부모 먼저는 방향과 무관하다.
                const bool Ascending = S.orders_.isEmpty() || S.orders_.front() == Qt::AscendingOrder;
                const auto Count = std::min(Left.tree_.size(), Right.tree_.size());
                for (std::size_t Index = 0; Index < Count; ++Index) {
                    const auto& [LF, LV] = Left.tree_[Index]; const auto& [RF, RV] = Right.tree_[Index];
                    if (LF != RF) return !LF;
                    const int Order = S.options_.locale_ ? LV.key_->compare(*RV.key_) : S.plain_(LV.string_, RV.string_);
                    if (Order) return Ascending ? Order < 0 : Order > 0;
                }
                if (Left.tree_.size() != Right.tree_.size()) return Left.tree_.size() < Right.tree_.size();
                return QString::compare(Left.id_, Right.id_, Qt::CaseSensitive) < 0;
            }
            if (Left.directory_ != Right.directory_) return Left.directory_;
            const auto Count = std::min<qsizetype>(S.orders_.size(), qsizetype(std::min(Left.values_.size(), Right.values_.size())));
            for (qsizetype Index = 0; Index < Count; ++Index) {
                const auto& LV = Left.values_[Index]; const auto& RV = Right.values_[Index];
                if (LV.ready_ != RV.ready_) return LV.ready_;
                if (!LV.ready_) continue;
                int Order = 0;
                if (LV.text_ && RV.text_) Order = S.options_.locale_ ? LV.key_->compare(*RV.key_) : S.plain_(LV.string_, RV.string_);
                else {
                    const auto LeftRaw = LV.text_ ? QVariant(LV.string_) : LV.raw_, RightRaw = RV.text_ ? QVariant(RV.string_) : RV.raw_;
                    const auto Compared = QVariant::compare(LeftRaw, RightRaw); Order = Compared < 0 ? -1 : Compared > 0 ? 1 : 0;
                }
                if (Order) return S.orders_[Index] == Qt::AscendingOrder ? Order < 0 : Order > 0;
            }
            return QString::compare(Left.id_, Right.id_, Qt::CaseSensitive) < 0;
        }
        // 한 작업을 반영한다. 상태가 어긋나면 false.
        static bool apply(State& S, Job& Item)
        {
            if (Item.reset_ || Item.generation_ != S.generation_ || S.orders_ != Item.orders_ || !(S.options_ == Item.options_)) {
                if (!Item.reset_ || Item.first_ != 0) { S.Release(); S.generation_ = 0; return false; }
                S.Release(); S.generation_ = Item.generation_; S.orders_ = Item.orders_; S.options_ = Item.options_;
                S.collator_.emplace(); S.collator_->setNumericMode(S.options_.numeric_); S.collator_->setCaseSensitivity(S.options_.case_);
                S.plain_ = TextCompare(false, S.options_.numeric_, S.options_.case_);
            }
            if (Item.first_ != int(S.rows_.size())) { S.Release(); S.generation_ = 0; return false; }
            const auto First = S.rows_.size();
            S.rows_.reserve(First + Item.rows_.size());
            for (auto& Row : Item.rows_) S.rows_.push_back(keyed(S, std::move(Row)));
            Item.rows_ = {};
            const auto Begin = S.order_.size();
            for (auto Row = First; Row < S.rows_.size(); ++Row) S.order_.push_back(int(Row));
            const auto Less = [&S](int A, int B) { return less(S, S.rows_[A], S.rows_[B]); };
            std::stable_sort(S.order_.begin() + qsizetype(Begin), S.order_.end(), Less);
            if (Begin) std::inplace_merge(S.order_.begin(), S.order_.begin() + qsizetype(Begin), S.order_.end(), Less);
            return true;
        }
        static void run(const std::shared_ptr<State>& Shared)
        {
            auto& S = *Shared;
            for (;;) {
                Job Item;
                { std::scoped_lock Lock(S.mutex_);
                  // Clear는 호출 시점에 대기열을 비웠다. 그 뒤에 들어온 작업은 해제 후에 그대로 처리한다.
                  if (S.releasePending_) { S.Release(); S.generation_ = 0; S.releasePending_ = false; }
                  if (S.queue_.empty()) { S.running_ = false; return; }
                  Item = std::move(S.queue_.front()); S.queue_.pop_front(); }
                const auto Generation = Item.generation_;
                const bool Applied = apply(S, Item);
                bool Deliver = true;
                { std::scoped_lock Lock(S.mutex_);
                  // 같은 세대의 다음 묶음이 기다리면 그것까지 병합한 뒤 한 번만 알린다.
                  if (Applied && !S.queue_.empty() && S.queue_.front().generation_ == Generation && !S.queue_.front().reset_) Deliver = false;
                  if (Generation < S.latest_) Deliver = false; }
                if (!Deliver) continue;
                QVector<int> Ranks;
                if (Applied) {
                    Ranks.resize(qsizetype(S.order_.size()));
                    for (qsizetype Rank = 0; Rank < Ranks.size(); ++Rank) Ranks[S.order_[Rank]] = int(Rank);
                }
                const int Rows = Applied ? int(S.rows_.size()) : 0;
                std::weak_ptr<State> Weak = Shared;
                auto* App = QCoreApplication::instance(); if (!App) continue;
                QMetaObject::invokeMethod(App, [Weak, Generation, Rows, Ranks = std::move(Ranks)]() mutable {
                    const auto Alive = Weak.lock(); if (!Alive) return;
                    Ready Callback;
                    { std::scoped_lock Lock(Alive->mutex_); if (Generation < Alive->latest_) return; Callback = Alive->ready_; }
                    if (Callback) Callback(Generation, Rows, std::move(Ranks));
                }, Qt::QueuedConnection);
            }
        }
        std::shared_ptr<State> state_;
    };
}
