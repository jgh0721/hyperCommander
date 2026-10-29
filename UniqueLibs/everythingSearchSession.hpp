#pragma once
#include "quickFindSettings.hpp"
#include "storageProviders.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QDir>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSet>
#include <QThreadPool>
#include <QTimer>
#include <QtConcurrent>
#include <deque>
#include <functional>
#include <utility>

namespace hc::search {
// One SDK query lives in its own helper. The bounded shared pool also bounds
// native calls left running by cancelled searches (notably disconnected UNC).
class EverythingSession final : public QObject {
public:
    using Entries = std::vector<storage::EntrySnapshot>;
    std::function<void(Entries)> append_;
    std::function<void(const QString&)> status_;
    EverythingSession(QObject* Parent, std::shared_ptr<storage::IStorageProvider> Local,
        std::shared_ptr<storage::IStorageProvider> Smb)
        : QObject(Parent), local_(std::move(Local)), smb_(std::move(Smb))
    {
        setObjectName("hc.everything.session");
        flush_.setSingleShot(true);
        connect(&flush_, &QTimer::timeout, this, [this] { publish(); });
    }
    ~EverythingSession() override { Cancel(); }
    void Start(const QString& Query, const QString& Match, int Interval, const QString& Helper = {})
    {
        interval_ = Interval; query_ = Query; lastFlush_.start();
        process_ = new QProcess(this);
        process_->setProgram(Helper.isEmpty() ? QCoreApplication::applicationDirPath() + "/hc-search-index.exe" : Helper);
        process_->setArguments({"--quick-find-stream", Query, Match});
        connect(process_, &QProcess::readyReadStandardOutput, this, [this] { receive(); });
        connect(process_, &QProcess::readyReadStandardError, this, [this] { process_->readAllStandardError(); });
        connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError Error) {
            if (Error == QProcess::FailedToStart) fail(tr("검색 도우미를 시작할 수 없습니다."));
        });
        connect(process_, &QProcess::finished, this, [this](int Code, QProcess::ExitStatus Exit) {
            receive();
            if (!cancelled_ && failure_.isEmpty() && (Code || Exit != QProcess::NormalExit || !ended_ || !buffer_.isEmpty()))
                fail(tr("Everything 검색 응답이 완료되지 않았습니다. Everything 실행 상태를 확인하십시오."));
            inputDone_ = true; pump(); schedule(); updateStatus();
        });
        process_->start(); updateStatus();
        QTimer::singleShot(30000, this, [this] {
            if (!cancelled_ && !snapshot_ && !inputDone_) fail(tr("Everything 응답 대기 시간이 초과되었습니다."));
        });
    }
    void SetDisplayInterval(int Interval)
    {
        interval_ = Interval;
        if (!ready_.empty()) { flush_.stop(); schedule(); }
    }
    void Cancel()
    {
        if (cancelled_) return;
        cancelled_ = true; stop_.request_stop(); flush_.stop();
        pending_.clear(); ready_.clear();
        if (process_) {
            auto* Process = std::exchange(process_, nullptr); Process->disconnect(this); Process->setParent(nullptr);
            if (Process->state() == QProcess::NotRunning) Process->deleteLater();
            else { connect(Process, &QProcess::finished, Process, &QObject::deleteLater); Process->kill(); }
        }
    }
    bool Running() const { return !cancelled_ && (!inputDone_ || active_ || !pending_.empty() || !ready_.empty()); }
private:
    struct Prepared { Entries entries_; quint64 failed_ = 0; };
    static QThreadPool& workers()
    {
        static QThreadPool Pool; static const bool Initialized = [] { Pool.setMaxThreadCount(4); return true; }();
        Q_UNUSED(Initialized); return Pool;
    }
    void fail(const QString& Message)
    {
        if (cancelled_) return;
        failure_ = Message; inputDone_ = true;
        if (process_ && process_->state() != QProcess::NotRunning) process_->kill();
        updateStatus();
    }
    void receive()
    {
        if (cancelled_ || !process_) return;
        buffer_ += process_->readAllStandardOutput();
        for (;;) {
            const auto End = buffer_.indexOf('\n'); if (End < 0) break;
            if (End > 256 * 1024) { buffer_.clear(); fail(tr("검색 응답 메시지가 너무 큽니다.")); return; }
            const auto Line = buffer_.left(End); buffer_.remove(0, End + 1);
            QJsonParseError Error; const auto Doc = QJsonDocument::fromJson(Line, &Error);
            const auto Value = Doc.object();
            if (Error.error != QJsonParseError::NoError || !Doc.isObject() || Value["v"] != 1
                || !Value["seq"].isDouble() || Value["seq"].toDouble() != double(sequence_++) || ended_) {
                buffer_.clear(); fail(tr("검색 응답 형식이 올바르지 않습니다.")); return;
            }
            const auto Type = Value["type"].toString();
            if (Type == "started" && sequence_ == 1) continue;
            if (Type == "snapshot" && !snapshot_) {
                bool Ok = false; total_ = Value["total"].toString().toULongLong(&Ok);
                if (!Ok) { fail(tr("검색 결과 개수가 올바르지 않습니다.")); return; }
                snapshot_ = true;
            } else if (Type == "items" && snapshot_ && Value["paths"].isArray()) {
                const auto Paths = Value["paths"].toArray();
                if (Paths.isEmpty()) { fail(tr("빈 검색 응답 묶음입니다.")); return; }
                for (const auto& Item : Paths) {
                    const auto Path = Item.toString();
                    if (!Item.isString() || !QDir::isAbsolutePath(Path) || Path.contains(QChar::Null)) {
                        fail(tr("검색 결과 경로가 올바르지 않습니다.")); return;
                    }
                    ++received_;
                    if (!seen_.contains(Path)) { seen_.insert(Path); pending_.push_back(Path); pendingBytes_ += Path.size() * 2; }
                }
                acknowledgements_.push_back(sequence_ - 1);
            } else if (Type == "error" && snapshot_) {
                fail(tr("결과 수신 중 Everything 목록이 변경되었거나 페이지를 읽지 못했습니다. 검색을 다시 실행하십시오.")); return;
            } else if (Type == "end" && snapshot_) {
                bool Ok = false; const auto Count = Value["emitted"].toString().toULongLong(&Ok);
                if (!Ok || Count != received_ || Count != total_ || !Value["complete"].toBool()) { fail(tr("검색 결과의 종료 개수가 일치하지 않습니다.")); return; }
                ended_ = true;
            } else { fail(tr("예상하지 않은 검색 응답입니다.")); return; }
        }
        if (buffer_.size() > 256 * 1024) { buffer_.clear(); fail(tr("검색 응답 메시지가 너무 큽니다.")); return; }
        pump(); acknowledge(); schedule(); updateStatus();
    }
    void acknowledge()
    {
        if (!process_ || pendingBytes_ > 4 * 1024 * 1024 || pending_.size() > 4096 || ready_.size() > 1024) return;
        while (!acknowledgements_.empty()) {
            const auto Seq = acknowledgements_.front(); acknowledgements_.pop_front();
            process_->write(QJsonDocument(QJsonObject{{"v", 1}, {"type", "ack"}, {"seq", double(Seq)}}).toJson(QJsonDocument::Compact) + '\n');
        }
    }
    void pump()
    {
        if (cancelled_) return;
        while (active_ < 4 && !pending_.empty() && ready_.size() < 1024) {
            QStringList Paths;
            while (Paths.size() < 16 && !pending_.empty()) {
                pendingBytes_ -= pending_.front().size() * 2; Paths.append(std::move(pending_.front())); pending_.pop_front();
            }
            ++active_;
            auto* Watcher = new QFutureWatcher<Prepared>(this);
            connect(Watcher, &QFutureWatcher<Prepared>::finished, this, [this, Watcher] {
                auto Result = Watcher->result(); Watcher->deleteLater(); --active_;
                if (cancelled_) return;
                skipped_ += Result.failed_;
                for (auto& Entry : Result.entries_) ready_.push_back(std::move(Entry));
                pump(); acknowledge(); schedule(); updateStatus();
            });
            const auto Local = local_, Smb = smb_; const auto Stop = stop_.get_token();
            Watcher->setFuture(QtConcurrent::run(&workers(), [Paths = std::move(Paths), Local, Smb, Stop] {
                Prepared Result;
                for (const auto& Path : Paths) {
                    if (Stop.stop_requested()) break;
                    const auto Native = QDir::toNativeSeparators(Path);
                    const bool Network = Native.startsWith("\\\\?\\UNC\\", Qt::CaseInsensitive)
                        || (Native.startsWith("\\\\") && !Native.startsWith("\\\\?\\"));
                    storage::OperationContext Context; Context.stopToken_ = Stop;
                    auto Value = (Network ? Smb : Local)->Resolve({Network ? storage::LocatorKind::Smb : storage::LocatorKind::Local, Native.toStdString()}, Context).get();
                    if (Value.HasValue()) {
                        Value.Value().hints_["filesystemPath"] = Native.toStdString();
                        Result.entries_.push_back(std::move(Value.Value()));
                    } else ++Result.failed_;
                }
                return Result;
            }));
        }
    }
    void schedule()
    {
        if (cancelled_ || ready_.empty() || flush_.isActive()) return;
        const bool Last = inputDone_ && pending_.empty() && active_ == 0;
        flush_.start(!published_ || Last ? 0 : int(qMax<qint64>(0, interval_ - lastFlush_.elapsed())));
    }
    void publish()
    {
        if (cancelled_ || ready_.empty()) return;
        auto Items = std::move(ready_); ready_.clear(); published_ += Items.size();
        if (append_) append_(std::move(Items));
        lastFlush_.restart(); pump(); acknowledge(); schedule(); updateStatus();
    }
    void updateStatus()
    {
        if (!status_ || cancelled_) return;
        const auto Progress = !snapshot_ && !inputDone_ ? tr("Everything 검색 중…") : Running() ? tr("Everything 결과 추가 중…")
            : !failure_.isEmpty() ? tr("Everything 부분 결과") : tr("Everything 검색 완료");
        auto Text = tr("%1 · 일치 %2개 · 표시 %3개").arg(Progress).arg(total_).arg(published_);
        if (skipped_) Text += tr(" · 파일 확인 실패 %1개").arg(skipped_);
        if (!failure_.isEmpty()) Text += " · " + failure_;
        status_(Text);
    }
    std::shared_ptr<storage::IStorageProvider> local_, smb_;
    QProcess* process_ = nullptr;
    QTimer flush_;
    QElapsedTimer lastFlush_;
    std::stop_source stop_;
    QByteArray buffer_;
    QString query_, failure_;
    QSet<QString> seen_;
    std::deque<QString> pending_;
    std::deque<quint64> acknowledgements_;
    Entries ready_;
    quint64 total_ = 0, received_ = 0, published_ = 0, skipped_ = 0, sequence_ = 0;
    qsizetype pendingBytes_ = 0;
    int active_ = 0, interval_ = 500;
    bool snapshot_ = false, ended_ = false, inputDone_ = false, cancelled_ = false;
};
}
