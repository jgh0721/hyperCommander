#pragma once
#include "storageProviders.hpp"
#include "ProductContants.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QScopeGuard>
#include <QThreadPool>
#include <QTimer>
#include <QtConcurrent>
#include <atomic>
#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <set>

// 평면 보기: 기준 폴더 아래의 항목을 너비 우선으로 열거해 한 목록에 누적한다.
// 반영 규칙은 Everything 결과(EverythingSession)와 같다. 첫 묶음과 마지막 묶음은 바로,
// 나머지는 표시 주기마다 넘긴다. 항목 수 한도는 두지 않는다(취소로만 멈춘다).
namespace hc::flat {
enum class Items { Files, FilesAndFolders, Folders };
struct ListingOptions {
    // 열거할 폴더. 기준 폴더 하나이거나, 기준 폴더 안에서 선택한 폴더들이다.
    std::vector<storage::Locator> roots_;
    // 상대 경로의 기준(네이티브 구분자, 끝 구분자 없음. 드라이브 루트는 "C:\").
    QString base_;
    Items items_ = Items::Files;
    // 각 루트 아래로 내려갈 단계 수. 0은 루트 자신의 항목만이다.
    int maxDepth_ = std::numeric_limits<int>::max();
    // 정션 · 심볼릭 링크 폴더를 따라간다. 방문한 파일 ID로 순환을 막는다(평면 옵션).
    bool followReparse_ = false;
};
struct SkippedFolder { QString path_, reason_; };
struct Status {
    quint64 folders_ = 0, found_ = 0, published_ = 0;
    std::vector<SkippedFolder> skipped_;
    bool running_ = false, cancelled_ = false, completed_ = false;
    QString failure_;
};
// 기준 폴더 기준 상대 위치. Path는 항목의 네이티브 전체 경로다.
inline QString NormalizedBase(QString Base)
{
    Base = QDir::toNativeSeparators(Base);
    while (Base.size() > 3 && Base.endsWith('\\')) Base.chop(1);
    if (Base.size() == 2 && Base[1] == ':') Base += '\\';
    return Base;
}
// 기준 폴더에 대한 부모 폴더의 상대 경로(기준 폴더의 항목은 빈 문자열).
inline QString RelativeParent(const QString& Base, const QString& Path)
{
    const auto Prefix = Base.endsWith('\\') ? Base.size() : Base.size() + 1;
    const auto Slash = Path.lastIndexOf('\\');
    if (Slash < Prefix || !Path.startsWith(Base, Qt::CaseInsensitive)) return {};
    return Path.mid(Prefix, Slash - Prefix);
}
// 상대 부모 경로의 구간 수 = 기준 폴더에서 내려간 단계(기준 폴더의 항목은 0).
// UTF-8 경로에서 '\'는 한 바이트이므로 바이트로 센다.
inline int RelativeDepth(std::size_t BaseBytes, const std::string& Path)
{
    if (Path.size() <= BaseBytes + 1) return 0;
    return int(std::count(Path.begin() + qsizetype(BaseBytes) + (Path[BaseBytes] == '\\' ? 1 : 0), Path.end(), '\\'));
}

class ListingSession final : public QObject {
public:
    using Entries = std::vector<storage::EntrySnapshot>;
    std::function<void(Entries)> append_;
    std::function<void(const Status&)> status_;
    ListingSession(QObject* Parent, std::shared_ptr<storage::IStorageProvider> Provider, ListingOptions Options)
        : QObject(Parent), provider_(std::move(Provider)), options_(std::move(Options)), shared_(std::make_shared<Shared>())
    {
        setObjectName("hc.flat.session");
        flush_.setSingleShot(true);
        connect(&flush_, &QTimer::timeout, this, [this] { publish(); });
        // 작업 스레드는 이 객체를 건드리지 않는다. UI 스레드가 공유 상태를 짧은 주기로 비운다.
        drain_.setInterval(40);
        connect(&drain_, &QTimer::timeout, this, [this] { drain(); });
    }
    ~ListingSession() override { Cancel(); }
    void Start(int Interval = hc::product::flatViewDisplayIntervalMs)
    {
        interval_ = Interval; lastFlush_.start(); current_.running_ = true;
        const auto Shared = shared_; const auto Provider = provider_; const auto Options = options_;
        const auto Stop = shared_->stop_.get_token();
        (void)QtConcurrent::run(&workers(), [Shared, Provider, Options, Stop] { enumerate(*Shared, Provider, Options, Stop); });
        drain_.start(); updateStatus();
    }
    void SetDisplayInterval(int Interval)
    {
        interval_ = Interval;
        if (!ready_.empty()) { flush_.stop(); schedule(); }
    }
    void Cancel()
    {
        if (cancelled_) return;
        cancelled_ = true; shared_->stop_.request_stop(); flush_.stop(); drain_.stop();
        ready_.clear();
        if (current_.running_) { current_.running_ = false; current_.cancelled_ = true; }
    }
    // 열거 중지 후에도 그때까지의 결과는 목록에 남는다. 상태 표시만 "중지됨"으로 바꾼다.
    void Stop()
    {
        if (cancelled_ || !current_.running_) return;
        drain(); publish();
        shared_->stop_.request_stop(); drain_.stop(); flush_.stop(); ready_.clear(); cancelled_ = true;
        current_.running_ = false; current_.cancelled_ = true; notify();
    }
    bool Running() const { return !cancelled_ && current_.running_; }
    const Status& CurrentStatus() const { return current_; }
    const ListingOptions& Options() const { return options_; }
private:
    struct Shared {
        std::stop_source stop_;
        std::mutex mutex_;
        std::deque<Entries> batches_;
        std::vector<SkippedFolder> skipped_;
        std::atomic<quint64> folders_ = 0, found_ = 0;
        std::atomic<bool> done_ = false;
        QString failure_;
    };
    static QThreadPool& workers()
    {
        static QThreadPool Pool; static const bool Initialized = [] { Pool.setMaxThreadCount(4); return true; }();
        Q_UNUSED(Initialized); return Pool;
    }
    static QString describe(const storage::Error& Error)
    {
        using storage::ErrorCategory;
        switch (Error.category_) {
        case ErrorCategory::AccessDenied: return QCoreApplication::translate("FlatView", "접근 거부");
        case ErrorCategory::NotFound: return QCoreApplication::translate("FlatView", "찾을 수 없음");
        case ErrorCategory::Unavailable: return QCoreApplication::translate("FlatView", "사용할 수 없음");
        default: return QString::fromStdString(Error.code_);
        }
    }
    static void enumerate(Shared& State, std::shared_ptr<storage::IStorageProvider> Provider, ListingOptions Options, std::stop_token Stop)
    {
        using namespace storage;
        const auto Finish = qScopeGuard([&] { State.done_ = true; });
        if (!Provider) { std::scoped_lock Lock(State.mutex_); State.failure_ = QCoreApplication::translate("FlatView", "위치를 읽을 수 없습니다."); return; }
        OperationContext Context; Context.stopToken_ = Stop; Context.maxPageItems_ = 256;
        // 열람과 같은 방식: 자식 하나의 메타데이터 오류로 부모 목록 전체가 실패하지 않는다.
        Context.allowUnavailableChildMetadata_ = true;
        std::deque<std::pair<Locator, int>> Pending;
        for (const auto& Root : Options.roots_) Pending.push_back({Root, 0});
        std::set<std::string> Visited;
        Entries Batch; QElapsedTimer Since; Since.start();
        const auto Post = [&](bool Force) {
            if (Batch.empty() || (!Force && Batch.size() < 2048 && Since.elapsed() < 30)) return;
            { std::scoped_lock Lock(State.mutex_); State.batches_.push_back(std::move(Batch)); }
            Batch = {}; Since.restart();
        };
        const bool Files = Options.items_ != Items::Folders, Folders = Options.items_ != Items::Files;
        while (!Pending.empty() && !Stop.stop_requested()) {
            auto [Location, Depth] = std::move(Pending.front()); Pending.pop_front();
            const bool IsRoot = Depth == 0;
            auto Open = Provider->Enumerate(Location, Context).get();
            if (!Open.HasValue()) {
                if (Open.Failure().category_ == ErrorCategory::Cancelled || Stop.stop_requested()) break;
                std::scoped_lock Lock(State.mutex_);
                if (IsRoot && Options.roots_.size() == 1) { State.failure_ = describe(Open.Failure()); break; }
                State.skipped_.push_back({QString::fromStdString(Location.root_), describe(Open.Failure())});
                continue;
            }
            ++State.folders_;
            auto Cursor = std::move(Open.Value());
            for (;;) {
                auto Read = Cursor->Next(256, Context).get();
                if (!Read.HasValue()) {
                    if (Read.Failure().category_ != ErrorCategory::Cancelled && !Stop.stop_requested()) {
                        std::scoped_lock Lock(State.mutex_);
                        State.skipped_.push_back({QString::fromStdString(Location.root_), describe(Read.Failure())});
                    }
                    break;
                }
                for (auto& Entry : Read.Value().entries_) {
                    const bool Reparse = (Entry.attributes_ & 0x400) != 0;
                    if (Entry.directory_ && Depth < Options.maxDepth_ && (!Reparse || Options.followReparse_)) {
                        // 정션을 따라갈 때만 순환을 막는다. 일반 폴더 트리는 같은 경로를 두 번 만나지 않는다.
                        if (!Reparse || Visited.insert(Entry.locator_.root_).second) Pending.push_back({Entry.locator_, Depth + 1});
                    }
                    if (Entry.directory_ ? !Folders : !Files) continue;
                    Entry.hints_["filesystemPath"] = Entry.locator_.root_;
                    Batch.push_back(std::move(Entry)); ++State.found_;
                }
                Post(false);
                if (Read.Value().end_ || Stop.stop_requested()) break;
            }
        }
        Post(true);
    }
    void drain()
    {
        if (cancelled_) return;
        std::deque<Entries> Batches; std::vector<SkippedFolder> Skipped; QString Failure;
        { std::scoped_lock Lock(shared_->mutex_);
          Batches.swap(shared_->batches_);
          if (shared_->skipped_.size() > current_.skipped_.size())
              Skipped.assign(shared_->skipped_.begin() + qsizetype(current_.skipped_.size()), shared_->skipped_.end());
          Failure = shared_->failure_; }
        for (auto& Batch : Batches) for (auto& Entry : Batch) ready_.push_back(std::move(Entry));
        for (auto& Item : Skipped) current_.skipped_.push_back(std::move(Item));
        current_.folders_ = shared_->folders_; current_.found_ = shared_->found_;
        current_.failure_ = Failure;
        if (shared_->done_ && Batches.empty()) {
            // 완료 판정은 마지막 묶음을 받은 다음 주기에 한다(작업 스레드의 마지막 묶음 이후 done_).
            std::scoped_lock Lock(shared_->mutex_);
            if (shared_->batches_.empty()) { inputDone_ = true; drain_.stop(); }
        }
        schedule(); notify();
    }
    void schedule()
    {
        if (cancelled_ || flush_.isActive()) return;
        if (ready_.empty()) { if (inputDone_) finish(); return; }
        flush_.start(!published_ || inputDone_ ? 0 : int(qMax<qint64>(0, interval_ - lastFlush_.elapsed())));
    }
    void publish()
    {
        if (cancelled_ || ready_.empty()) return;
        auto Items = std::move(ready_); ready_ = {}; published_ += Items.size(); current_.published_ = published_;
        if (append_) append_(std::move(Items));
        lastFlush_.restart(); schedule(); notify();
    }
    void finish()
    {
        if (!current_.running_) return;
        current_.running_ = false; current_.completed_ = current_.failure_.isEmpty();
        notify();
    }
    void notify()
    {
        if (!status_ || (cancelled_ && !current_.cancelled_)) return;
        const auto Time = statusTimer_.isValid() ? statusTimer_.elapsed() : 1000;
        // 상태 표시줄은 진행 중에 200ms마다, 끝나면 바로 갱신한다.
        if (current_.running_ && Time < 200) return;
        statusTimer_.start(); status_(current_);
    }
    void updateStatus() { statusTimer_.invalidate(); notify(); }
    std::shared_ptr<storage::IStorageProvider> provider_;
    ListingOptions options_;
    std::shared_ptr<Shared> shared_;
    QTimer flush_, drain_;
    QElapsedTimer lastFlush_, statusTimer_;
    Entries ready_;
    Status current_;
    quint64 published_ = 0;
    int interval_ = hc::product::flatViewDisplayIntervalMs;
    bool cancelled_ = false, inputDone_ = false;
};
}
