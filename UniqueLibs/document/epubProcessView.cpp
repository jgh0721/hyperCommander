#include "stdafx.h"
#include "epubProcessView.hpp"
#include "commonLibs/document/epub/epubAppearance.hpp"
#include "UniqueLibs/settings/appSettings.hpp"

/// Internal libraries
#include "commonLibs/document/epub/epubPackage.hpp"
#include "commonLibs/document/epub/epubInputSender.hpp"

/// Platform/framework headers
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <QApplication>
#include <QDeadlineTimer>
#include <QEvent>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QPointer>
#include <QTimer>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

namespace hc::document
{
    struct EpubProcessView::Work
    {
        QByteArray bytes_;
        qint64 generation_ = 1;
        NativeBinding native_;
        std::uintptr_t parent_ = 0;
        WebProcessSnapshot snapshot_;
        std::atomic<bool> cancel_ = false, done_ = false, ready_ = false, attached_ = false;
        std::atomic<bool> focusRequested_ = false;
        mutable std::mutex mutex_;
        QJsonObject state_{{"status", "opening"}};
        QJsonObject metrics_;
        QJsonObject appearance_;
        QString settingsPath_;
        std::deque<QJsonObject> settingsRequests_;
        QJsonObject settingsResult_;
        void Metrics(QJsonObject Value) { std::lock_guard Lock(mutex_); metrics_ = std::move(Value); }
        std::thread worker_;
        void Update(QJsonObject Value) { std::lock_guard Lock(mutex_); state_ = std::move(Value); }
    };
    struct EpubProcessView::BindingState
    {
        NativeBinding token_;
        QPointer<EpubProcessView> host_;
        HWND hostWindow_ = nullptr, proxy_ = nullptr;
        std::shared_ptr<Work> work_;
        bool valid_ = true, retired_ = false, attached_ = false;
    };
    class EpubProcessView::Registry final : public QObject
    {
    public:
        static Registry& Instance()
        {
            auto& Value = instance();
            if(!Value) Value = new Registry;
            return *Value;
        }
        static void ReleaseLifecycle()
        {
            auto& Value = instance();
            if(Value && Value->Drained()) delete Value.data();
        }
        std::shared_ptr<BindingState> Add(EpubProcessView* Host, const std::shared_ptr<Work>& Work)
        {
            if(shutdown_ || !parking_) return {};
            const auto HostWindow = reinterpret_cast<HWND>(Host->winId());
            auto Entry = std::make_shared<BindingState>();
            Entry->token_ = {++sequence_, 1}; Entry->host_ = Host;
            Work->native_ = Entry->token_;
            Entry->hostWindow_ = HostWindow; Entry->work_ = Work;
            // Launch only against a stable parking-owned proxy. The view can die
            // during startup without freeing/recycling the HWND sent to Start.
            Entry->proxy_ = CreateWindowExW(WS_EX_NOPARENTNOTIFY, L"STATIC", L"", WS_CHILD | WS_CLIPCHILDREN,
                0, 0, Host->width(), Host->height(), parking_, nullptr, GetModuleHandleW(nullptr), nullptr);
            if(!Entry->proxy_) return {};
            if(!SetPropW(Entry->proxy_, property_, reinterpret_cast<HANDLE>(Entry.get()))) {
                DestroyWindow(Entry->proxy_); return {};
            }
            Work->parent_ = reinterpret_cast<std::uintptr_t>(Entry->proxy_);
            entries_.push_back(Entry); timer_.start(); return Entry;
        }
        bool Current(const std::shared_ptr<BindingState>& Entry, NativeBinding Token) const
        {
            return Entry && Entry->valid_ && !Entry->retired_ && Entry->token_ == Token
                && Entry->host_ && Entry->hostWindow_ == reinterpret_cast<HWND>(Entry->host_->internalWinId())
                && owns(*Entry) && GetParent(Entry->proxy_) == (Entry->attached_ ? Entry->hostWindow_ : parking_);
        }
        void Invalidate(const std::shared_ptr<BindingState>& Entry)
        {
            if(!Entry || !Entry->valid_) return;
            Entry->valid_ = false; ++Entry->token_.generation_; Entry->work_->cancel_.store(true);
            // This never changes the helper's immediate parent HWND. If external
            // destruction has already removed the proxy, do not touch its reuse.
            if(owns(*Entry)) { ShowWindow(Entry->proxy_, SW_HIDE); SetParent(Entry->proxy_, parking_); }
        }
        void Shutdown()
        {
            shutdown_ = true;
            for(const auto& Entry : entries_) Invalidate(Entry);
        }
        bool Drained() const { return entries_.empty(); }
        ~Registry() override
        {
            // QApplication::exit()/destruction without a drained event loop is a
            // caller contract violation. Never turn it into a successful ACK.
            Q_ASSERT(entries_.empty());
            if(parking_) DestroyWindow(parking_);
        }
    protected:
        bool eventFilter(QObject* Object, QEvent* Event) override
        {
            if(Object == qApp && Event->type() == QEvent::Quit && !Drained()) {
                quitPending_ = true; Shutdown(); Event->ignore(); return true;
            }
            return QObject::eventFilter(Object, Event);
        }
    private:
        static QPointer<Registry>& instance()
        {
            static QPointer<Registry> Value;
            return Value;
        }
        Registry() : QObject(qApp)
        {
            parking_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"", WS_POPUP,
                0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            qApp->installEventFilter(this); timer_.setInterval(10);
            connect(&timer_, &QTimer::timeout, this, [this] { collect(); });
        }
        static bool owns(const BindingState& Entry)
        {
            return Entry.proxy_ && GetPropW(Entry.proxy_, property_) == reinterpret_cast<HANDLE>(const_cast<BindingState*>(&Entry));
        }
        void collect()
        {
            for(auto At = entries_.begin(); At != entries_.end();) {
                const auto Entry = *At;
                if(!Current(Entry, Entry->token_) || Entry->work_->cancel_.load()) Invalidate(Entry);
                if(Entry->valid_ && !Entry->attached_ && Entry->work_->ready_.load(std::memory_order_acquire)) {
                    const auto& Snapshot = Entry->work_->snapshot_;
                    const auto Child = reinterpret_cast<HWND>(Snapshot.childWindow_);
                    DWORD Pid = 0; GetWindowThreadProcessId(Child, &Pid);
                    if(!Snapshot.authenticated_ || Pid != Snapshot.processId_ || GetParent(Child) != Entry->proxy_
                        || !SetParent(Entry->proxy_, Entry->hostWindow_)) Invalidate(Entry);
                    else {
                        ShowWindow(Entry->proxy_, SW_SHOWNOACTIVATE);
                        Entry->attached_ = true; Entry->work_->attached_.store(true, std::memory_order_release);
                    }
                }
                if(Entry->valid_ && Entry->attached_) {
                    RECT Rect{};
                    if(!GetClientRect(Entry->hostWindow_, &Rect)
                        || !SetWindowPos(Entry->proxy_, nullptr, 0, 0, Rect.right, Rect.bottom, SWP_NOACTIVATE | SWP_NOZORDER))
                        Invalidate(Entry);
                }
                if(!Entry->work_->done_.load(std::memory_order_acquire)) { ++At; continue; }
                auto& Worker = Entry->work_->worker_;
                if(Worker.joinable()) {
                    if(WaitForSingleObject(Worker.native_handle(), 0) != WAIT_OBJECT_0) { ++At; continue; }
                    Worker.join(); // Already signalled: no live worker wait on the GUI.
                }
                if(Entry->valid_) { Entry->valid_ = false; ++Entry->token_.generation_; }
                if(owns(*Entry)) { RemovePropW(Entry->proxy_, property_); DestroyWindow(Entry->proxy_); }
                Entry->proxy_ = nullptr; Entry->retired_ = true; At = entries_.erase(At);
            }
            if(entries_.empty()) {
                timer_.stop();
                if(quitPending_) { quitPending_ = false; QTimer::singleShot(0, qApp, &QCoreApplication::quit); }
            }
        }
        static constexpr const wchar_t* property_ = L"HyperCommander.EpubProxyOwner";
        HWND parking_ = nullptr;
        QTimer timer_;
        std::vector<std::shared_ptr<BindingState>> entries_;
        quint64 sequence_ = 0;
        bool shutdown_ = false, quitPending_ = false;
    };

    EpubProcessView::EpubProcessView(WebProcessRuntime Runtime, WebProcessLimits Limits, QWidget* Parent)
        : QWidget(Parent), runtime_(std::move(Runtime)), limits_(Limits), timer_(new QTimer(this))
    {
        const auto Changed = [this] {
            if(work_) { const auto Value = epubui::Appearance(); std::lock_guard Lock(work_->mutex_); work_->appearance_ = Value; }
        };
        connect(&fm::style::ThemeManager::instance(), &fm::style::ThemeManager::changed, this, Changed);
        connect(&fm::style::ThemeManager::instance(), &fm::style::ThemeManager::fontsChanged, this, Changed);
        timer_->setInterval(20);
        connect(timer_, &QTimer::timeout, this, &EpubProcessView::collect);
    }
    EpubProcessView::~EpubProcessView() { RequestClose(); }
    void EpubProcessView::InitializeLifecycle() { Registry::Instance(); }
    void EpubProcessView::RequestShutdown() { Registry::Instance().Shutdown(); }
    bool EpubProcessView::IsDrained() { return Registry::Instance().Drained(); }
    void EpubProcessView::ReleaseLifecycle() { Registry::ReleaseLifecycle(); }
    void EpubProcessView::RequestClose()
    {
        if(work_) work_->cancel_.store(true);
        if(native_ && !native_->retired_) Registry::Instance().Invalidate(native_);
    }
    bool EpubProcessView::ChangeReadingSettings(const QString& Action,const QJsonObject& Profile,bool Global) {
        if(!work_ || !native_ || !native_->valid_ || !native_->attached_ || work_->cancel_.load() || work_->done_.load()
            || !QStringList{"preview","apply","cancel","reload"}.contains(Action)
            || QJsonDocument(Profile).toJson(QJsonDocument::Compact).size()>65536)return false;
        std::lock_guard Lock(work_->mutex_);if(work_->state_["status"]!="ready" || work_->settingsRequests_.size()>=16)return false;
        work_->settingsRequests_.push_back({{"kind","reading-settings"},{"action",Action},{"profile",Profile},{"global",Global}});return true;
    }
    bool EpubProcessView::FocusInput()
    {
        if(!work_ || !isVisible() || work_->cancel_.load() || work_->done_.load()) return false;
        work_->focusRequested_.store(true);
        return true;
    }
    void EpubProcessView::collect()
    {
        const auto State = ViewState();
        if(State == reported_) return;
        const bool Escape=State["escapeRequests"].toInt()>reported_["escapeRequests"].toInt();
        reported_ = State;
        const QPointer<EpubProcessView> Guard(this);
        const bool Retired = State["retired"].toBool();
        if(Retired) timer_->stop();
        if(!loaded_ && !work_->cancel_.load() && State["attached"].toBool() && State["status"] == "ready") {
            loaded_ = true;
            emit Loaded();
            if(!Guard) return;
        }
        if(Retired && State["status"] == "error" && !failed_) {
            failed_ = true;
            emit Failed(State["message"].toString());
            if(!Guard) return;
        }
        if(Retired && !closed_) {
            closed_ = true;
            emit Closed();
            if(!Guard) return;
        }
        if(Escape && !Retired) {emit EscapeRequested();if(!Guard)return;}
        emit ViewStateChanged();
    }
    bool EpubProcessView::event(QEvent* Event)
    {
        if(work_ && Event->type() == QEvent::Hide) work_->focusRequested_.store(false);
        if(native_ && !native_->retired_ && (Event->type() == QEvent::ParentAboutToChange || Event->type() == QEvent::WinIdChange))
            RequestClose();
        return QWidget::event(Event);
    }
    bool EpubProcessView::ReaderAction(const QString& Action,const QJsonValue& Value) {
        if(!work_ || closed_ || failed_ || !bound_ || !QStringList{"search","locate","bookmark-add","bookmark-remove","image","footnote","close-popup","focus-search"}.contains(Action)
            || QJsonDocument(QJsonArray{Value}).toJson(QJsonDocument::Compact).size()>16384)return false;
        std::lock_guard Lock(work_->mutex_);if(work_->state_["status"]!="ready" || work_->settingsRequests_.size()>=16)return false;
        work_->settingsRequests_.push_back({{"kind","reader-action"},{"action",Action},{"value",Value}});return true;
    }
    QJsonObject EpubProcessView::ViewState() const
    {
        if(!work_) return {{"status", "idle"}};
        std::lock_guard Lock(work_->mutex_);
        auto Result = work_->state_;
        Result["metrics"] = work_->metrics_;
        Result["settingsResult"] = work_->settingsResult_;
        Result["attached"] = native_ && native_->valid_ && native_->attached_;
        Result["retired"] = native_ ? native_->retired_ : work_->done_.load();
        return Result;
    }
    bool EpubProcessView::Load(QByteArray Bytes, qint64 Generation)
    {
        if(bound_) return false;
        bound_ = true;
        timer_->start();
        work_ = std::make_shared<Work>();
        work_->appearance_ = epubui::Appearance();
        auto Config = qApp->property("documentViewsConfigDir").toString();
        if(Config.isEmpty()) Config = hc::settings::DocumentPaths().directory_;
        work_->settingsPath_ = QDir(Config).absoluteFilePath("epub-reading.json");
        if((runtime_.kind_ != WebProcessKind::EpubPrototype && runtime_.kind_ != WebProcessKind::EpubReadOnly) || Generation < 1 || Generation > 9007199254740991LL
            || Bytes.isEmpty() || Bytes.size() > epub::InputLimit) {
            work_->Update({{"status", "error"}, {"message", "epub-view-admission"}});
            work_->done_.store(true); return false;
        }
        work_->bytes_ = std::move(Bytes); work_->generation_ = Generation;
        native_ = Registry::Instance().Add(this, work_);
        if(!native_) {
            work_->Update({{"status", "error"}, {"message", "epub-native-binding-unavailable"}});
            work_->done_.store(true); return false;
        }
        try {
            work_->worker_ = std::thread([State = work_, Runtime = std::move(runtime_), Limits = limits_] { run(State, Runtime, Limits); });
        } catch(const std::system_error&) {
            work_->Update({{"status", "error"}, {"message", "epub-worker-start"}});
            work_->done_.store(true, std::memory_order_release); return false;
        }
        return true;
    }
    void EpubProcessView::run(const std::shared_ptr<Work>& State, WebProcessRuntime Runtime, WebProcessLimits Limits)
    {
        std::unique_ptr<DocumentWebProcess> Owner, Pending;
        WebProcessError Error;
        QString Failure;
        QJsonObject LastReaderState;
        QByteArray Payload; // Retain input until proven retirement.
        QElapsedTimer Elapsed; Elapsed.start();
        QJsonObject Metrics{{"processLimit", int(Limits.processes_)}, {"jobMemoryLimit", qint64(Limits.memoryBytes_)}};
        quint64 PeakBytes = 0;
        quint32 PeakProcesses = 0;
        const auto Cancelled = [&] { return State->cancel_.load(); };
        try {
            const auto Process = [&] {
                QDeadlineTimer Deadline(60000);
                auto Package = epub::Prepare(State->bytes_, Cancelled);
                Metrics["prepareMs"] = Elapsed.elapsed();
                if(!Package) { Failure = Package.error_; return; }
                if(Cancelled()) return;
                const auto Id = Package.publication_["sourceSha256"].toString();
                Payload = QJsonDocument(Package.publication_).toJson(QJsonDocument::Compact);
                Owner = DocumentWebProcess::Start(Runtime, Limits, State->parent_, Error, &Pending, Cancelled);
                if(!Owner) { Failure = Error.code_; return; }
                Metrics["authenticatedMs"] = Elapsed.elapsed();
                State->snapshot_ = Owner->Snapshot();
                State->ready_.store(true, std::memory_order_release);
                while(!State->attached_.load(std::memory_order_acquire) && !Cancelled() && !Deadline.hasExpired())
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                if(Cancelled()) return;
                if(Deadline.hasExpired()) { Failure = "epub-view-attach-timeout"; return; }
                QJsonObject Applied;
                { std::lock_guard Lock(State->mutex_); Applied = State->appearance_; }
                const auto Configure = QJsonDocument(QJsonObject{{"protocolVersion", 1}, {"documentId", Id},
                    {"generation", State->generation_}, {"kind", "configure"}, {"appearance", Applied}, {"settingsPath", State->settingsPath_}}).toJson(QJsonDocument::Compact);
                QByteArray ConfigureReply;
                if(!Owner->Exchange(Configure, ConfigureReply, 5000, Error)
                    || !QJsonDocument::fromJson(ConfigureReply).object()["accepted"].toBool()) { Failure = "epub-configuration-rejected"; return; }
                auto Sent = epub::SendInput(Payload, Id, State->generation_, "epubjs",
                    [&](const QByteArray& Request, QByteArray& Reply, int Timeout) {
                        return Owner->Exchange(Request, Reply, Timeout, Error);
                    }, Cancelled);
                if(!Sent.accepted_) { Failure = Sent.error_; return; }
                const auto Request = QJsonDocument(QJsonObject{{"protocolVersion", 1}, {"documentId", Id},
                    {"generation", State->generation_}, {"kind", "snapshot"}}).toJson(QJsonDocument::Compact);
                bool DocumentReady = false;
                bool FocusAcknowledged = false;
                while(!Cancelled()) {
                    QJsonObject Next;
                    { std::lock_guard Lock(State->mutex_); Next = State->appearance_; }
                    if(Next != Applied) {
                        const auto ThemeRequest = QJsonDocument(QJsonObject{{"protocolVersion", 1}, {"documentId", Id},
                            {"generation", State->generation_}, {"kind", "appearance"}, {"appearance", Next}}).toJson(QJsonDocument::Compact);
                        QByteArray ThemeReply;
                        if(!Owner->Exchange(ThemeRequest, ThemeReply, 5000, Error)
                            || !QJsonDocument::fromJson(ThemeReply).object()["accepted"].toBool()) { Failure = "epub-appearance-rejected"; return; }
                        Applied = Next;
                    }
                    QJsonObject Settings;
                    {std::lock_guard Lock(State->mutex_);if(!State->settingsRequests_.empty()){Settings=std::move(State->settingsRequests_.front());State->settingsRequests_.pop_front();}}
                    if(!Settings.isEmpty()) {
                        Settings["protocolVersion"]=1;Settings["documentId"]=Id;Settings["generation"]=State->generation_;
                        QByteArray Reply;
                        if(!Owner->Exchange(QJsonDocument(Settings).toJson(QJsonDocument::Compact),Reply,5000,Error)){Failure=Error.code_;return;}
                        QJsonParseError SettingsError;const auto SettingsDocument=QJsonDocument::fromJson(Reply,&SettingsError);
                        auto Result=SettingsDocument.object();
                        if(SettingsError.error!=QJsonParseError::NoError || !SettingsDocument.isObject()
                            || Result["protocolVersion"]!=QJsonValue(1) || Result["documentId"]!=QJsonValue(Id)
                            || Result["generation"]!=QJsonValue(State->generation_)) {Failure="epub-settings-reply-identity";return;}
                        Result["action"]=Settings["action"];
                        {std::lock_guard Lock(State->mutex_);State->settingsResult_=Result;}
                    }
                    QByteArray Bytes;
                    if(!Owner->Exchange(Request, Bytes, 5000, Error)) { Failure = Error.code_; return; }
                    QJsonParseError ParseError;
                    const auto Document = QJsonDocument::fromJson(Bytes, &ParseError);
                    const auto Reply = Document.object();
                    if(ParseError.error != QJsonParseError::NoError || !Document.isObject()
                        || Reply["protocolVersion"] != QJsonValue(1) || Reply["documentId"] != QJsonValue(Id)
                        || Reply["generation"] != QJsonValue(State->generation_) || !Reply["state"].isObject() || Reply.contains("error")) {
                        Failure = "epub-view-snapshot-identity"; return;
                    }
                    auto View = Reply["state"].toObject();
                    LastReaderState = View;
                    const auto Status = View["status"].toString();
                    if(Status == "error" || Status == "cancelled") { Failure = View["message"].toString("epub-renderer-failed"); return; }
                    if(!DocumentReady && Status == "ready") Metrics["readyMs"] = Elapsed.elapsed();
                    DocumentReady = DocumentReady || Status == "ready";
                    const auto Usage = Owner->Snapshot();
                    PeakBytes = qMax(PeakBytes, Usage.peakJobBytes_);
                    PeakProcesses = qMax(PeakProcesses, Usage.activeProcesses_);
                    Metrics["peakJobBytes"] = qint64(PeakBytes);
                    Metrics["peakObservedProcesses"] = int(PeakProcesses);
                    Metrics["activeProcesses"] = int(Usage.activeProcesses_);
                    State->Metrics(Metrics);
                    if(DocumentReady && State->focusRequested_.exchange(false) && !Cancelled()
                        && IsWindowVisible(reinterpret_cast<HWND>(State->parent_))) {
                        auto Focus = QJsonDocument::fromJson(Request).object(); Focus["kind"] = "focus";
                        QByteArray FocusBytes;
                        if(!Owner->Exchange(QJsonDocument(Focus).toJson(QJsonDocument::Compact), FocusBytes, 5000, Error)) {
                            Failure = Error.code_; return;
                        }
                        const auto Ack = QJsonDocument::fromJson(FocusBytes).object();
                        if(Ack["protocolVersion"] != QJsonValue(1) || Ack["documentId"] != QJsonValue(Id)
                            || Ack["generation"] != QJsonValue(State->generation_) || Ack["accepted"] != QJsonValue(true)
                            || !Ack["focusRequested"].isBool() || Ack.contains("error")) {
                            Failure = "epub-view-focus-identity"; return;
                        }
                        FocusAcknowledged = Ack["focusRequested"].toBool();
                    }
                    View["focusAcknowledged"] = FocusAcknowledged;
                    View["processId"] = int(State->snapshot_.processId_);
                    State->Update(std::move(View));
                    if(!DocumentReady && Deadline.hasExpired()) { Failure = "epub-view-load-timeout"; return; }
                    for(int I = 0; I < 10 && !Cancelled(); ++I) std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            };
            Process();
        } catch(...) { Failure = "epub-view-worker-exception"; }
        const auto ClosingAt = Elapsed.elapsed();
        State->Update({{"status", "closing"}});
        while((Owner && !Owner->Close(100)) || (Pending && !Pending->Close(100)))
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if(Owner) {
            const auto Usage = Owner->Snapshot();
            Metrics["peakJobBytes"] = qint64(qMax(PeakBytes, Usage.peakJobBytes_));
            Metrics["activeProcesses"] = int(Usage.activeProcesses_);
        } else Metrics["activeProcesses"] = 0;
        Owner.reset(); Pending.reset();
        Metrics["closeMs"] = Elapsed.elapsed() - ClosingAt;
        State->Metrics(Metrics);
        State->Update({{"status", Failure.isEmpty() || Cancelled() ? "closed" : "error"},
            {"message", Failure}, {"activeProcessesAfterClose", 0},
            {"lastReaderState", Failure.isEmpty() ? QJsonObject{} : LastReaderState}});
        State->done_.store(true, std::memory_order_release);
    }
}
