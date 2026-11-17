#pragma once

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QFutureWatcher>
#include <QTimer>
#include <QtConcurrentRun>
#include <deque>
#include <functional>
#include <utility>

namespace hc::rst {
    // Preserve protocol order while preparing large full-text notifications off-thread.
    // Only adjacent full replacements for the same document may supersede one another:
    // requests, incremental changes, save and close messages are ordering barriers.
    class LspWriteQueue final : public QObject {
    public:
        using Encoder = std::function<QByteArray()>;
        using Sink = std::function<void(const QByteArray&)>;
        explicit LspWriteQueue(Sink Write, QObject* Parent = nullptr) : QObject(Parent), write_(std::move(Write)) {}
        void Enqueue(QByteArray Frame) {
            pending_.push_back({{}, {}, std::move(Frame)}); drain();
        }
        void Enqueue(Encoder Encode, QString ReplaceKey = {}) {
            Packet Next{std::move(ReplaceKey), std::move(Encode), {}};
            if (!Next.key_.isEmpty() && !pending_.empty() && pending_.back().key_ == Next.key_)
                pending_.back() = std::move(Next);
            else pending_.push_back(std::move(Next));
            drain();
        }
        void Reset() { ++generation_; pending_.clear(); active_ = false; }
        bool Idle() const { return !active_ && pending_.empty(); }
    private:
        struct Packet { QString key_; Encoder encode_; QByteArray bytes_; };
        std::deque<Packet> pending_;
        Sink write_;
        quint64 generation_ = 0;
        bool active_ = false;
        void drain() {
            if (active_) return;
            const auto Generation = generation_;
            for (int Count = 0; !pending_.empty() && Count < 64; ++Count) {
                auto Next = std::move(pending_.front()); pending_.pop_front();
                if (Next.encode_) {
                    active_ = true;
                    auto* Watcher = new QFutureWatcher<QByteArray>(this);
                    connect(Watcher, &QFutureWatcherBase::finished, this, [this, Watcher, Generation] {
                        const auto Frame = Watcher->result(); Watcher->deleteLater();
                        if (Generation != generation_) return;
                        // Keep active while delivering: a reentrant enqueue must remain behind pending frames.
                        write_(Frame);
                        if (Generation != generation_) return;
                        active_ = false; drain();
                    });
                    Watcher->setFuture(QtConcurrent::run(std::move(Next.encode_)));
                    return;
                }
                active_ = true; write_(Next.bytes_);
                if (Generation != generation_) return;
                active_ = false;
            }
            if (!pending_.empty()) {
                active_ = true;
                QTimer::singleShot(0, this, [this, Generation] {
                    if (Generation != generation_) return;
                    active_ = false; drain();
                });
            }
        }
    };
}
