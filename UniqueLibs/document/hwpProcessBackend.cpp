#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "hwpStartupTrace.hpp"
#include <Windows.h>
#include "hwpProcessBackend.hpp"
#include "hwpInputSender.hpp"
#include <QDeadlineTimer>
#include <QFileInfo>
#include <QJsonParseError>
#include <QApplication>
#include <QEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>
#include <QKeyEvent>
#include <QKeySequence>
#include <deque>
#include <atomic>
#include <chrono>
#include <system_error>
#include <thread>
#include <mutex>
#include <utility>
#include <vector>

namespace hc::document
{
    struct HwpProcessBackend::Work
    {
        DocumentOperationTicket ticket_;
        SourceIdentity source_;
        HwpNativeBinding native_;
        int timeoutMs_ = 30000;
        std::unique_ptr<HwpAdmittedInput> input_;
        ExternalModelRevision model_;
        bool editable_ = false;
        std::mutex editorMutex_;
        QJsonObject command_, reply_;
        std::deque<QString> navigation_;
        bool focusRequested_=false;
        std::atomic<bool> documentReady_ = false;
        std::uintptr_t parent_ = 0;
        WebProcessSnapshot snapshot_;
        std::atomic<bool> cancel_ = false, done_ = false, ready_ = false, attached_ = false;
        QString error_;
        QString retirementError_; // GUI-thread only; worker owns error_.
        std::thread worker_; // GUI-owned handle; registry joins only after native exit proof.
    };
    struct HwpProcessBackend::BindingState
    {
        HwpNativeBinding token_;
        QPointer<HwpProcessBackend> host_;
        HWND hostWindow_ = nullptr, proxy_ = nullptr;
        std::shared_ptr<Work> work_;
        bool valid_ = true, retired_ = false, attached_ = false;
    };
    class HwpProcessBackend::Registry final : public QObject
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
        std::shared_ptr<BindingState> Add(HwpProcessBackend* Host, const std::shared_ptr<Work>& Work)
        {
            if(shutdown_ || !parking_) return {};
            const auto HostWindow = reinterpret_cast<HWND>(Host->winId());
            auto Entry = std::make_shared<BindingState>();
            Entry->token_ = {++sequence_, 1}; Entry->host_ = Host;
            Work->native_ = Entry->token_;
            Entry->hostWindow_ = HostWindow; Entry->work_ = Work;
            // Launch only against a stable parking-owned proxy. The SDI can die
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
        bool Current(const std::shared_ptr<BindingState>& Entry, HwpNativeBinding Token) const
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
        static constexpr const wchar_t* property_ = L"HyperCommander.RhwpProxyOwner";
        HWND parking_ = nullptr;
        QTimer timer_;
        std::vector<std::shared_ptr<BindingState>> entries_;
        quint64 sequence_ = 0;
        bool shutdown_ = false, quitPending_ = false;
    };
    HwpProcessBackend::HwpProcessBackend(WebProcessRuntime Runtime, WebProcessLimits Limits, int LoadTimeoutMs, bool Editable)
        : runtime_(std::move(Runtime)), limits_(Limits), timer_(new QTimer(this)), loadTimeoutMs_(LoadTimeoutMs), editable_(Editable)
    {
        timer_->setInterval(10);
        if(!editable_)setFocusPolicy(Qt::StrongFocus);
        connect(timer_, &QTimer::timeout, this, &HwpProcessBackend::collect);
    }
    HwpProcessBackend::~HwpProcessBackend()
    {
        if(native_) Registry::Instance().Invalidate(native_);
        if(work_) work_->cancel_.store(true);
    }
    HwpNativeBinding HwpProcessBackend::NativeBinding() const { return native_ ? native_->token_ : HwpNativeBinding{}; }
    bool HwpProcessBackend::IsCurrentBinding(HwpNativeBinding Binding) const { return Registry::Instance().Current(native_, Binding); }
    void HwpProcessBackend::InvalidateBinding(HwpNativeBinding Binding)
    {
        if(IsCurrentBinding(Binding)) Registry::Instance().Invalidate(native_);
    }
    void HwpProcessBackend::InitializeLifecycle() { Registry::Instance(); }
    void HwpProcessBackend::ReleaseLifecycle() { Registry::ReleaseLifecycle(); }
    void HwpProcessBackend::RequestShutdown() { Registry::Instance().Shutdown(); }
    bool HwpProcessBackend::IsDrained() { return Registry::Instance().Drained(); }
    bool HwpProcessBackend::event(QEvent* Event)
    {
        if(native_ && (Event->type() == QEvent::ParentAboutToChange || Event->type() == QEvent::WinIdChange))
            Registry::Instance().Invalidate(native_);
        return HwpSdiBackend::event(Event);
    }
    void HwpProcessBackend::Load(quint64 Binding, DocumentOperationTicket Ticket, SourceIdentity Source)
    {
        if(bound_) return;
        bound_ = true; binding_ = Binding;
        work_ = std::make_shared<Work>(); work_->ticket_ = std::move(Ticket); work_->source_ = std::move(Source);
        work_->timeoutMs_ = loadTimeoutMs_; work_->editable_ = editable_;
        native_ = Registry::Instance().Add(this, work_);
        if(!native_) {
            work_->error_ = "rhwp-native-binding-unavailable"; work_->done_.store(true, std::memory_order_release);
        } else try {
            if(startup::Enabled()) startup::Data().SetBinding({{"documentId",QString::fromStdString(work_->ticket_.documentId_)},
                {"generation",QString::number(work_->ticket_.generation_)},{"token",QString::number(work_->ticket_.token_)},
                {"bindingOwner",QString::number(work_->native_.owner_)},{"bindingGeneration",QString::number(work_->native_.generation_)},
                {"epoch",QString::number(work_->native_.owner_)}});
            work_->worker_ = std::thread([State = work_, Runtime = std::move(runtime_), Limits = limits_] { run(State, Runtime, Limits); });
        } catch(const std::system_error&) {
            work_->error_ = "rhwp-worker-start"; work_->done_.store(true, std::memory_order_release);
        }
        timer_->start();
    }
    bool HwpProcessBackend::OwnsLoad(quint64 Binding, const DocumentOperationTicket& Ticket, HwpNativeBinding Native) const
    {
        return bound_ && Binding == binding_ && work_ && Ticket == work_->ticket_
            && IsCurrentBinding(Native) && !work_->done_.load(std::memory_order_acquire);
    }
    bool HwpProcessBackend::RequestRetirement(quint64 Binding, const DocumentOperationTicket& Ticket,
        HwpNativeBinding Native, const QString& FailureCode)
    {
        if(!OwnsLoad(Binding, Ticket, Native)) return false;
        if(!FailureCode.isEmpty() && FailureCode != "rhwp-load-timeout"
            && FailureCode != "rhwp-native-load" && FailureCode != "rhwp-renderer-load") return false;
        work_->retirementError_ = FailureCode;
        Registry::Instance().Invalidate(native_);
        return true;
    }
    void HwpProcessBackend::Cancel(quint64 Binding, DocumentOperationTicket Ticket)
    {
        if(bound_ && Binding == binding_ && work_ && Ticket == work_->ticket_) {
            work_->cancel_.store(true); Registry::Instance().Invalidate(native_);
        }
    }
    void HwpProcessBackend::Stop(quint64 Binding)
    {
        if(!bound_) {
            bound_ = true; binding_ = Binding;
            QTimer::singleShot(0, this, [this, Binding] { emit Stopped(Binding); });
        } else if(Binding == binding_ && work_) {
            work_->cancel_.store(true); Registry::Instance().Invalidate(native_);
        }
    }
    void HwpProcessBackend::keyPressEvent(QKeyEvent* Event)
    {
        const auto Modifiers=Event->modifiers() & ~Qt::KeypadModifier;
        if(!editable_ && (Modifiers==Qt::NoModifier
            || (Modifiers==Qt::ControlModifier && (Event->key()==Qt::Key_Home || Event->key()==Qt::Key_End)))
            && QList<int>{Qt::Key_Home,Qt::Key_End,Qt::Key_PageUp,Qt::Key_PageDown,
                Qt::Key_Up,Qt::Key_Down,Qt::Key_Left,Qt::Key_Right}.contains(Event->key())
            && work_ && loadReported_ && IsCurrentBinding(work_->native_)
            && !work_->cancel_.load() && !work_->done_.load()) {
            std::lock_guard Lock(work_->editorMutex_);
            work_->navigation_.push_back(QKeySequence(Event->key()).toString(QKeySequence::PortableText));
            Event->accept();return;
        }
        HwpSdiBackend::keyPressEvent(Event);
    }
    void HwpProcessBackend::FocusInput()
    {
        if(!editable_ || !work_ || !loadReported_ || !IsCurrentBinding(work_->native_)
            || work_->cancel_.load() || work_->done_.load())return;
        std::lock_guard Lock(work_->editorMutex_);work_->focusRequested_=true;
    }
    bool HwpProcessBackend::EditorCommand(const QJsonObject& Request)
    {
        if ((!editable_ && !Request["type"].toString().startsWith("DocumentPrint")) || !work_ || !loadReported_ || !IsCurrentBinding(work_->native_)
            || work_->cancel_.load() || work_->done_.load()) return false;
        const auto Type = Request["type"].toString();
        if (Type != "DocumentPrint" && Type != "DocumentPrintCancel" && Type != "DocumentEditorExport" && Type != "DocumentEditorSaved" && Type != "DocumentEditorDiscard" && Type != "DocumentEditorSynchronize" && Type != "DocumentEditorResume") return false;
        std::lock_guard Lock(work_->editorMutex_);
        if (!work_->command_.isEmpty()) return false;
        work_->command_ = Request; return true;
    }
    void HwpProcessBackend::run(const std::shared_ptr<Work>& State, WebProcessRuntime Runtime, WebProcessLimits Limits)
    {
        WebProcessError Error;
        std::unique_ptr<DocumentWebProcess> Pending;
        if(State->timeoutMs_ < 1 || State->timeoutMs_ > 120000) {
            State->error_ = "rhwp-load-timeout-policy";
            State->done_.store(true, std::memory_order_release); return;
        }
        QDeadlineTimer Deadline(State->timeoutMs_, Qt::PreciseTimer);
        const auto Active = [&] {
            if(State->cancel_.load()) return false;
            if(!State->documentReady_.load() && Deadline.hasExpired()) {
                State->error_ = "rhwp-load-timeout"; return false;
            }
            return true;
        };
        const auto Budget = [&] {
            return State->documentReady_.load() ? quint32{3000}
                : static_cast<quint32>(qBound<qint64>(qint64{1}, Deadline.remainingTime(), qint64{3000}));
        };
        if(Active()) {
            Limits.startupMs_ = qMin(Limits.startupMs_, static_cast<quint32>(qMax<qint64>(1, Deadline.remainingTime())));
            auto Owner = DocumentWebProcess::Start(Runtime, Limits, State->parent_, Error, &Pending, [&] { return !Active(); });
            if(!Owner) {
                if(State->error_.isEmpty()) State->error_ = Error.code_ + (Error.childExitCode_ ? "-exit-" + QString::number(*Error.childExitCode_) : QString{});
            }
            else {
                State->snapshot_ = Owner->Snapshot();
                State->ready_.store(true, std::memory_order_release);
                startup::Scope Attach("editor.attachWait");
                while(Active() && !State->attached_.load(std::memory_order_acquire))
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                Attach.End();
                if(Active()) {
                    QByteArray Reply;
                    if(!Owner->Exchange(R"({"type":"RuntimeStatus","version":1})", Reply, Budget(), Error))
                        State->error_ = Error.code_ + (Error.childExitCode_ ? "-exit-" + QString::number(*Error.childExitCode_) : QString{});
                    else if(Active()) {
                        QJsonParseError Parse;
                        const auto Json = QJsonDocument::fromJson(Reply, &Parse);
                        const auto Status = Json.object();
                        if(Reply.size() > 4096 || Parse.error != QJsonParseError::NoError || !Json.isObject()
                            || Status.value("type") != QJsonValue("RuntimeStatus")
                            || Status.value("version") != QJsonValue(1)
                            || Status.value("transportReady") != QJsonValue(true)
                            || !Status.value("packageApproved").isBool()
                            || !Status.value("packageError").isString()
                            || Status.value("rendererCreated") != QJsonValue(false)
                            || !Status.value("documentCommands").isBool()
                            || !Status.value("inputTransfer").isBool())
                            State->error_ = "rhwp-runtime-status";
                        else if(!Status.value("packageApproved").toBool())
                            State->error_ = "rhwp-package-not-approved";
                        else if(Status.value("documentCommands") != QJsonValue(true)
                            || Status.value("inputTransfer") != QJsonValue(true))
                            State->error_ = "rhwp-document-bridge-unavailable";
                        else {
                            // The GUI registry has authenticated/attached this immutable
                            // owner binding; any subsequent invalidation sets cancel_.
                            const auto Name = State->source_.entry_ ? State->source_.entry_->name_ : State->source_.localPath_;
                            const auto Format = QFileInfo(QString::fromUtf8(Name)).suffix().toLower();
                            if(Active()) State->input_ = HwpAdmittedInput::Read(State->ticket_, State->source_,
                                State->native_.owner_, State->native_.generation_, State->native_.owner_, Format, State->error_);
                            if(State->input_ && Active() && State->input_->Send(*Owner,
                                [&] { return State->cancel_.load(); }, State->error_,
                                static_cast<int>(qMax<qint64>(1, Deadline.remainingTime()))) && Active()) {
                                const auto Identity = State->input_->Identity();
                                bool Started = false, Parsed = false;
                                int TraceAttempts = 0;
                                startup::Data().Begin("editor.documentStart");
                                while(Active()) {
                                    const QJsonObject Request{{"version", 1}, {"identity", Identity},
                                        {"type", Started ? "DocumentStatus" : State->editable_ ? "DocumentStartEdit" : "DocumentStart"}};
                                    if(!Owner->Exchange(QJsonDocument(Request).toJson(QJsonDocument::Compact), Reply, Budget(), Error)) {
                                        State->error_ = Error.code_ + (Error.childExitCode_ ? "-exit-" + QString::number(*Error.childExitCode_) : QString{}); break;
                                    }
                                    if(!Active()) break;
                                    QJsonParseError StatusParse;
                                    const auto Document = QJsonDocument::fromJson(Reply, &StatusParse);
                                    const auto Result = Document.object();
                                    State->error_ = "rhwp-document-status";
                                    if(Reply.size() > 4096 || StatusParse.error != QJsonParseError::NoError || !Document.isObject()
                                        || Result.size() != (State->editable_ ? 6 : 5) || Result["version"] != QJsonValue(1)
                                        || Result["type"] != QJsonValue("DocumentStatus") || Result["identity"] != QJsonValue(Identity)
                                        || !Result["error"].isString() || !Result["state"].isString()) break;
                                    const auto Stage = Result["state"].toString();
                                    if(Stage == "failed") {
                                        const auto Code = Result["error"].toString();
                                        State->error_ = Code == "rhwp-load-timeout" || Code == "rhwp-renderer-surface"
                                            || Code == "rhwp-native-load" ? Code : QString("rhwp-renderer-load");
                                        break;
                                    }
                                    if(!Result["error"].toString().isEmpty()
                                        || (Stage != "loading" && Stage != "parsed" && Stage != "ready")
                                        || (Parsed && Stage == "loading")
                                        || (State->documentReady_.load() && Stage != "ready")) break;
                                    Parsed = Parsed || Stage == "parsed";
                                    State->error_.clear();
                                    if(!Started) {startup::Data().EndNamed("editor.documentStart");startup::Data().Begin("editor.documentWait");}
                                    Started = true;
                                    if(Stage == "ready" && !State->documentReady_.load()) {
                                        // Read-only snapshot identity is generated natively,
                                        // not accepted as a revision/savepoint from renderer JSON.
                                        { std::lock_guard Lock(State->editorMutex_);
                                          State->model_ = {State->native_.owner_, 0, State->source_.inputSha256_}; }
                                        startup::Data().EndNamed("editor.documentWait");
                                        startup::Data().Begin("editor.publishReady");
                                        State->documentReady_.store(true, std::memory_order_release);
                                    }
                                    if (State->documentReady_.load()) {
                                        const auto ApplyEditor = [&](const QJsonObject& Editor) {
                                        bool ValidSequence = false;
                                        const auto SequenceText = Editor["sequence"].toString();
                                        const auto Sequence = SequenceText.toULongLong(&ValidSequence);
                                        const auto Savepoint = Editor["savepoint"].toString();
                                        const auto Binding = Editor["binding"].toObject();
                                        if (Editor["version"] != QJsonValue(1) || !ValidSequence
                                            || QString::number(Sequence) != SequenceText || Savepoint.isEmpty() || Savepoint.size() > 128
                                            || Binding["documentId"] != Identity["documentId"] || Binding["generation"] != Identity["generation"]
                                            || Binding["bindingOwner"] != Identity["bindingOwner"] || Binding["bindingGeneration"] != Identity["bindingGeneration"]
                                            || Binding["epoch"] != Identity["epoch"]) { State->error_ = "rhwp-editor-state"; return false; }
                                        std::lock_guard Lock(State->editorMutex_);
                                        if (Sequence < State->model_.changeSequence_) { State->error_ = "rhwp-editor-sequence"; return false; }
                                        State->model_ = {State->native_.owner_, Sequence, Savepoint.toStdString()};
                                        return true;
                                        };
                                        if (State->editable_ && !ApplyEditor(Result["editor"].toObject())) break;
                                        QJsonObject EditorRequest;
                                        bool CanPoll = false;
                                        {
                                            std::lock_guard Lock(State->editorMutex_);
                                            CanPoll = State->reply_.isEmpty();
                                            if (CanPoll) {
                                                EditorRequest = std::exchange(State->command_, {});
                                                if(EditorRequest.isEmpty() && std::exchange(State->focusRequested_,false))EditorRequest={{"type","DocumentFocus"}};
                                                if(EditorRequest.isEmpty() && !State->navigation_.empty()) {
                                                    EditorRequest={{"type","DocumentNavigate"},{"key",State->navigation_.front()}};
                                                    State->navigation_.pop_front();
                                                }
                                            }
                                        }
                                        if (CanPoll) {
                                            if (EditorRequest.isEmpty()) EditorRequest["type"] = State->editable_ ? "DocumentEditorPoll" : "DocumentPrintPoll";
                                            const auto Action = EditorRequest["type"].toString();
                                            EditorRequest["version"] = 1; EditorRequest["identity"] = Identity;
                                            if (!Owner->Exchange(QJsonDocument(EditorRequest).toJson(QJsonDocument::Compact), Reply, Budget(), Error)) {
                                                State->error_ = Error.code_ + (Error.childExitCode_ ? "-exit-" + QString::number(*Error.childExitCode_) : QString{}); break;
                                            }
                                            QJsonParseError EditorParse; const auto Response = QJsonDocument::fromJson(Reply, &EditorParse);
                                            if (EditorParse.error != QJsonParseError::NoError || !Response.isObject() || Reply.size() > 300 * 1024) {
                                                State->error_ = "rhwp-editor-reply"; break;
                                            }
                                            auto Out = Response.object(); Out["requestType"] = Action;
                                            // Poll can observe a newer edit than the preceding status.
                                            // Publish it before delivering a save action or export reply.
                                            if (Out["editor"].isObject() && !ApplyEditor(Out["editor"].toObject())) break;
                                            std::lock_guard Lock(State->editorMutex_); State->reply_ = std::move(Out);
                                        }
                                    }
                                        // Opt-in diagnostics only, after readiness publication. Bounded
                                        // memory export preserves the helper's existing hard Job retirement.
                                        if(State->documentReady_.load() && startup::Enabled() && TraceAttempts < 5) {
                                            ++TraceAttempts; QByteArray TraceReply; WebProcessError TraceError;
                                            if(Owner->Exchange(R"({"type":"RuntimeStartupTrace","version":1})",TraceReply,3000,TraceError)
                                                && TraceReply.size() <= 64*1024) {
                                                auto Trace = QJsonDocument::fromJson(TraceReply).object();
                                                if(Trace["schemaVersion"]==1 && Trace["runId"]==qEnvironmentVariable("HC_RHWP_STARTUP_TRACE_ID")
                                                    && Trace["pid"].toInteger()==State->snapshot_.processId_
                                                    && Trace["binding"]==QJsonValue(Identity) && Trace["rendererReceived"]==true) {
                                                    Trace["collectorPid"]=double(QCoreApplication::applicationPid());Trace["collectionAck"]=true;
                                                    if(!startup::Write(Trace,"helper",State->snapshot_.processId_)) startup::Data().MissingHelper("trace-output-write-failed");
                                                    TraceAttempts=5;
                                                }
                                            }
                                        }
                                    for(int Tick = 0; Tick < 10 && Active(); ++Tick)
                                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                                }
                            }
                        }
                    }
                }
                Pending = std::move(Owner);
            }
        }
        if(startup::Enabled()&&!State->documentReady_.load())
            startup::Data().MissingHelper(State->cancel_.load()?"cancelled-before-ready":State->error_);
        // Even failed startup can retain an unconfirmed process tree. Never
        // translate a timeout into Cancelled/Stopped or release the input lease.
        while(Pending && !Pending->Close(100))
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        Pending.reset();
        State->done_.store(true, std::memory_order_release);
    }
    void HwpProcessBackend::collect()
    {
        if(!work_) return;
        if(!loadReported_ && work_->documentReady_.load(std::memory_order_acquire)
            && !work_->cancel_.load() && !work_->done_.load(std::memory_order_acquire)
            && IsCurrentBinding(work_->native_)) {
            loadReported_ = true;
            ExternalModelRevision Model;
            { std::lock_guard Lock(work_->editorMutex_); Model = work_->model_; }
            reportedSequence_ = 0;
            // Initial load establishes the admitted byte baseline. Mutations
            // observed during delivery are published separately on the next tick.
            emit Loaded(binding_, work_->ticket_, ExternalModelRevision{Model.epoch_, 0, work_->source_.inputSha256_});
            // Direct observers may delete us or drain work_ in a nested loop.
            return;
        }
        if (loadReported_ && !work_->done_.load() && !work_->cancel_.load()
            && IsCurrentBinding(work_->native_)) {
            ExternalModelRevision Model; QJsonObject Reply;
            { std::lock_guard Lock(work_->editorMutex_); Model = work_->model_; Reply = std::exchange(work_->reply_, {}); }
            const QPointer<HwpProcessBackend> Guard(this);
            if (Model.changeSequence_ > reportedSequence_) {
                reportedSequence_ = Model.changeSequence_; emit ModelChanged(binding_, Model);
                if (!Guard) return;
            }
            if (!Reply.isEmpty()) { emit EditorReply(binding_, std::move(Reply)); return; }
        }
        if(!work_->done_.load(std::memory_order_acquire) || (native_ && !native_->retired_)) return;
        timer_->stop();
        const auto Work = std::move(work_); native_.reset();
        const auto Binding = binding_;
        QPointer<HwpProcessBackend> Guard(this);
        if(!loadReported_) {
            if(Work->cancel_.load() && Work->retirementError_.isEmpty()) emit Cancelled(Binding, Work->ticket_);
            else emit Loaded(Binding, Work->ticket_, storage::Error{storage::ErrorCategory::InvalidRequest,
                !Work->retirementError_.isEmpty() ? Work->retirementError_.toStdString()
                    : Work->error_.isEmpty() ? "rhwp-document-bridge-unavailable" : Work->error_.toStdString()});
        }
        if(Guard) emit Stopped(Binding);
    }
}
