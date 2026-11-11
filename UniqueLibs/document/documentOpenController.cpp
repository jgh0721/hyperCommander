#include "documentOpenController.hpp"
#include "commonLibs/document/epub/epubAppearance.hpp"
#include "documentHandlerMenu.hpp"
#include "pluginHost/wlxDetectionSettings.hpp"
#include "pluginHost/pluginApplicationRule.hpp"
#include "commonLibs/document/documentFormatSupport.hpp"
#include "commonLibs/document/documentHostProtocol.hpp"
#include "UniqueLibs/nativeMaterializer.hpp"
#include <QtConcurrent>
#include <QFutureWatcher>
#include <QProcess>
#include <QUuid>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QStringConverter>
#include <qt_windows.h>
#include <QCloseEvent>
#include <QTimer>
#include <QScopeGuard>
#include <algorithm>
#include "externalLibs/TotalCommander/tc_listplug.h"
namespace hc::document {
using namespace hc::storage;
namespace {
struct PreparedOpen {
    QString error_;
    QStringList paths_;
    std::vector<std::shared_ptr<IMaterializedFileLease>> leases_;
    std::shared_ptr<IMaterializer> materializer_;
};
Result<bool> TryUndeclaredWlx(const hc::plugins::PluginViewerOptions& Options,
    const std::shared_ptr<IContentSource>& Source, const QString& HiddenParent, OperationContext Context) {
    if (HiddenParent.isEmpty()) return false;
    auto Factory = MakeNativeMaterializer(QDir::tempPath().toStdString()); if (!Factory.HasValue()) return Factory.Failure();
    // ListLoad may inspect or edit its argument. Probe only a private copy,
    // never the user's original, and retain it until the isolated host exits.
    auto Lease = Factory.Value()->Materialize({Source}, Context).get(); if (!Lease.HasValue()) return Lease.Failure();
    const auto Path = QString::fromStdString(Lease.Value()->LocalPath());
    const auto Permissions = QFile::permissions(Path);
    if (!QFile::setPermissions(Path, QFileDevice::ReadOwner | QFileDevice::ReadUser)) return false;
    const auto Restore = qScopeGuard([&] { QFile::setPermissions(Path, Permissions); });
    auto Started = hc::plugins::PluginProcess::Start(Options.executable_, Options.manifestPath_, Options.approval_, Options.limits_, Context, Options.defaultIniPath_);
    if (!Started.HasValue()) return Started.Failure(); auto Host = std::move(Started.Value());
    auto Probed = Host->Probe(Context); if (!Probed.HasValue()) return Probed.Failure();
    auto Opened = Host->ViewerCall(hc::plugins::WireKind::ViewerOpen,
        {{"parent", HiddenParent}, {"file", Path}, {"width", 640}, {"height", 480}, {"flags", Options.flags_ & ~lcp_forceshow}}, Context);
    if (Opened.HasValue()) Host->ViewerCall(hc::plugins::WireKind::ViewerClose, {}, Context);
    Host->Close(); return Opened.HasValue();
}
bool LooksLikeText(const QByteArray& Sample) {
    // A prefix can end inside a UTF-8/CP949 sequence; trim only that boundary.
    for (int Trim = 0; Trim <= 3 && Trim <= Sample.size(); ++Trim) {
        const auto Bytes = Sample.left(Sample.size() - Trim);
        const bool Utf16Le = Bytes.startsWith("\xff\xfe"), Utf16Be = Bytes.startsWith("\xfe\xff");
        QStringDecoder Decoder(Utf16Le ? QStringConverter::Utf16LE : Utf16Be ? QStringConverter::Utf16BE : QStringConverter::Utf8);
        QString Text = Decoder(Bytes);
        if (Decoder.hasError() && !Utf16Le && !Utf16Be) {
            const auto Length = MultiByteToWideChar(949, MB_ERR_INVALID_CHARS, Bytes.constData(), int(Bytes.size()), nullptr, 0);
            if (!Length && !Bytes.isEmpty()) continue;
            Text.resize(Length);
            if (Length && MultiByteToWideChar(949, MB_ERR_INVALID_CHARS, Bytes.constData(), int(Bytes.size()),
                reinterpret_cast<wchar_t*>(Text.data()), Length) != Length) continue;
        } else if (Decoder.hasError()) continue;
        return std::none_of(Text.begin(), Text.end(), [](QChar Ch) {
            return Ch.unicode() < 32 && Ch != QChar('\t') && Ch != QChar('\r') && Ch != QChar('\n') && Ch != QChar('\f');
        });
    }
    return false;
}
}
bool ProbeDocumentText(IContentSource& Source, OperationContext Context) {
    if (Source.Describe().directory_ || Context.Check({})) return false;
    Context.maxReadBytes_ = std::min<std::size_t>(Context.maxReadBytes_, 64 * 1024);
    auto Stream = Source.OpenRead(Context).get(); if (!Stream.HasValue()) return false;
    QByteArray Sample;
    while (Sample.size() < 64 * 1024 && !Context.Check({})) {
        auto Chunk = Stream.Value()->Read(64 * 1024 - Sample.size(), Context).get();
        if (!Chunk.HasValue() || Chunk.Value().bytes_.size() > size_t(64 * 1024 - Sample.size())
            || (Chunk.Value().bytes_.empty() && !Chunk.Value().eof_)) return false;
        const auto& Bytes = Chunk.Value().bytes_;
        Sample.append(reinterpret_cast<const char*>(Bytes.data()), qsizetype(Bytes.size()));
        if (Chunk.Value().eof_ || Sample.size() == 64 * 1024) {
            if (Context.Check({}) || !Source.Verify(Context).get().HasValue()) return false;
            if (auto* Provider = dynamic_cast<ProviderContentSource*>(&Source)) {
                const auto* Data = reinterpret_cast<const std::byte*>(Sample.constData());
                Provider->RememberPrefix({Data, Data + Sample.size()}, Stream.Value());
            }
            return LooksLikeText(Sample);
        }
    }
    return false;
}
Result<HandlerSelection> ProbeDocumentHandlers(const DocumentHandlerCatalog& Catalog,
    const DocumentOpenRequest& Request, HandlerSelection Selection, OperationContext Context, const QString& HiddenParent) {
    using hc::plugins::PluginApplicationRule;
    for (const auto& [Id, Target] : Catalog.targets_) {
        if (Target.descriptor_.kind_ != HandlerKind::Plugin || Id.starts_with("builtin.")) continue;
        auto& Matches = Selection.handlerMatches_[Id]; Matches.assign(Selection.names_.size(), false);
        const auto Application = PluginApplicationRule::Parse(Target.plugin_.applicationRule_);
        if (!Application.HasValue() || !Application.Value().Enabled()) continue;
        std::optional<PluginApplicationRule> Detection;
        bool Inspected = false, Usable = true, NeedsLoad = false;
        for (std::size_t Index = 0; Index < Selection.names_.size(); ++Index) {
            if (const auto Stop = Context.Check({"document"})) return *Stop;
            if (!DocumentHandlerResolver::Supports(Target.descriptor_, Request.intent_, {Selection.names_[Index]})) continue;
            auto& Source = *Request.documents_[Index].source_;
            auto Applies = Application.Value().MatchesSource(Source, Context);
            if (!Applies.HasValue() || !Applies.Value()) continue;
            if (!Inspected && Target.plugin_.approval_.reviewed_.format_ == hc::plugins::PluginFormat::Wlx) {
                Inspected = true;
                auto Definition = hc::plugins::ReadWlxDetection(Target.plugin_, Context);
                if (!Definition.HasValue()) Usable = false;
                else {
                    const auto Text = Target.plugin_.detectionOverride_.value_or(Definition.Value());
                    // Without a declaration, TC calls ListLoad and continues if
                    // it returns NULL. Do that behind a hidden parent before
                    // advertising a candidate in the handler menu.
                    NeedsLoad = Text.trimmed().isEmpty();
                    if (!Text.trimmed().isEmpty()) {
                        auto Parsed = PluginApplicationRule::Parse(QJsonObject{{"version", 1}, {"mode", "tc"}, {"expression", Text}});
                        if (!Parsed.HasValue()) Usable = false;
                        else Detection = std::move(Parsed.Value());
                    }
                }
            }
            if (!Usable) continue;
            if (Detection) {
                auto Detected = Detection->MatchesSource(Source, Context);
                if (!Detected.HasValue() || !Detected.Value()) continue;
            }
            if (NeedsLoad) {
                auto Loaded = TryUndeclaredWlx(Target.plugin_, Request.documents_[Index].source_, HiddenParent, Context);
                if (!Loaded.HasValue() || !Loaded.Value()) continue;
            }
            Matches[Index] = true;
        }
    }
    for (const auto& Input : Request.documents_) {
        if (const auto Stop = Context.Check({"document"})) return *Stop;
        auto Verified = Input.source_->Verify(Context).get(); if (!Verified.HasValue()) return Verified.Failure();
    }
    return Selection;
}
DocumentOpenController::DocumentOpenController(QWidget* Main, Catalog Candidates, PluginLaunch Launch)
    : QObject(Main), main_(Main), catalog_(std::move(Candidates)), launch_(std::move(Launch)) {
    setObjectName("hc.document.openController"); Main->installEventFilter(this);
}
DocumentOpenController::~DocumentOpenController() { for (const auto& Stop : pending_) Stop->request_stop(); }
QString DocumentOpenController::Capture(DocumentOpenRequest Request) {
    const auto Token = QUuid::createUuid().toString(QUuid::Id128); requests_.insert(Token, std::move(Request));
    QTimer::singleShot(60000, this, [this, Token] { if (requests_.remove(Token)) report(Token, "cancelled", "document-request-expired"); }); return Token;
}
void DocumentOpenController::report(const QString& Token, const QString& Status, const QString& Reason) {
    cancelled_.remove(Token); emit Finished(Token, Status, Reason);
}
void DocumentOpenController::Cancel(const QString& Token) {
    if (requests_.remove(Token)) report(Token, "cancelled");
    else if (pending_.contains(Token)) { cancelled_.insert(Token); pending_[Token]->request_stop(); }
}
bool DocumentOpenController::Open(const QString& Token) {
    const auto At = requests_.find(Token); if (At == requests_.end()) return false;
    auto Request = std::move(At.value()); requests_.erase(At);
    if (!Request.stillCurrent_ || !Request.stillCurrent_()) { report(Token, "rejected", "document-context-changed"); return true; }
    HandlerSelection Selection;
    for (const auto& Input : Request.documents_) {
        if (!Input.source_) { Selection.providerSupported_ = false; break; }
        const auto Entry = Input.source_->Describe(); Selection.names_.push_back(Entry.name_); Selection.directory_ |= Entry.directory_;
        Selection.textContent_.push_back(false);
    }
    auto Catalog = catalog_();
    if (Selection.directory_ || !Selection.providerSupported_ || Selection.names_.empty())
        return selectHandler(Token, std::move(Request), std::move(Selection), std::move(Catalog));
    bool NeedProbe = false;
    for (const auto& Input : Request.documents_) if (Input.source_) {
        const auto Entry = Input.source_->Describe();
        const auto Ext = QFileInfo(QString::fromStdString(Entry.name_)).suffix().toLower().toStdString();
        NeedProbe |= std::find(DocumentExtensions.begin(), DocumentExtensions.end(), Ext) == DocumentExtensions.end();
    }
    for (const auto& [Id, Target] : Catalog.targets_)
        NeedProbe |= Target.descriptor_.kind_ == HandlerKind::Plugin && !Id.starts_with("builtin.") && Target.descriptor_.available_;
    if (!NeedProbe) return selectHandler(Token, std::move(Request), std::move(Selection), std::move(Catalog));
    if (!probeAnchor_) {
        probeAnchor_ = new QWidget(main_); probeAnchor_->setObjectName("hc.document.detectAnchor");
        probeAnchor_->resize(640, 480); probeAnchor_->hide();
    }
    const auto HiddenParent = QString::number(probeAnchor_->winId(), 16);
    // Read only a bounded prefix on a worker. A slow share must not block the panel.
    const auto Stop = std::make_shared<std::stop_source>(); pending_.insert(Token, Stop);
    auto* Watcher = new QFutureWatcher<Result<HandlerSelection>>(this);
    connect(Watcher, &QFutureWatcher<Result<HandlerSelection>>::finished, this, [this, Watcher, Token, Request, Catalog] {
        auto Probed = Watcher->result(); Watcher->deleteLater(); pending_.remove(Token);
        if (cancelled_.contains(Token)) { report(Token, "cancelled"); return; }
        if (!main_ || !Request.stillCurrent_()) { report(Token, "rejected", "document-context-changed"); return; }
        if (!Probed.HasValue()) { report(Token, "failed", QString::fromStdString(Probed.Failure().code_)); return; }
        selectHandler(Token, Request, std::move(Probed.Value()), Catalog);
    });
    Watcher->setFuture(QtConcurrent::run([Request, Catalog, Selection = std::move(Selection), Stop, HiddenParent]() mutable -> Result<HandlerSelection> {
        OperationContext Context; Context.stopToken_ = Stop->get_token();
        Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        Context.maxReadBytes_ = 64 * 1024;
        for (size_t Index = 0; Index < Selection.names_.size(); ++Index) {
            if (Context.Check({})) break;
            const auto& Source = Request.documents_[Index].source_;
            const auto Entry = Source->Describe();
            const auto Ext = QFileInfo(QString::fromStdString(Entry.name_)).suffix().toLower().toStdString();
            if (std::find(DocumentExtensions.begin(), DocumentExtensions.end(), Ext) != DocumentExtensions.end()) continue;
            Selection.textContent_[Index] = ProbeDocumentText(*Source, Context);
        }
        return ProbeDocumentHandlers(Catalog, Request, std::move(Selection), Context, HiddenParent);
    }));
    return true;
}
bool DocumentOpenController::selectHandler(const QString& Token, DocumentOpenRequest Request, HandlerSelection Selection, DocumentHandlerCatalog Catalog) {
    auto Default = Request.intent_ == DocumentIntent::View ? Catalog.defaultView_ : Catalog.defaultEdit_;
    if (!Selection.names_.empty()) {
        const auto Name = QString::fromStdString(Selection.names_.front()); const auto Ext = QFileInfo(Name).suffix().toLower();
        const auto Override = Catalog.overrides_[Ext].toObject()[Request.intent_ == DocumentIntent::View ? "view" : "edit"].toString();
        if (!Override.isEmpty()) Default = Override.toStdString();
    }
    const auto Resolution = DocumentHandlerResolver::Resolve(Catalog.registry_, Request.intent_, Selection, Default);
    if (Selection.directory_ || !Selection.providerSupported_ || Selection.names_.empty()) { report(Token, "rejected", QString::fromStdString(Resolution.reason_)); return true; }
    DocumentHandlerChoice Choice;
    if (Resolution.candidates_.size() == 1) {
        Choice.handler_ = Resolution.candidates_.front(); for (std::size_t I = 0; I < Request.documents_.size(); ++I) Choice.indexes_.push_back(I);
    } else {
        Catalog.defaultView_ = Request.intent_ == DocumentIntent::View ? Default : Catalog.defaultView_;
        Catalog.defaultEdit_ = Request.intent_ == DocumentIntent::Edit ? Default : Catalog.defaultEdit_;
        Choice = DocumentHandlerMenu::Choose(main_, Catalog, Request.intent_, Selection, Resolution);
    }
    if (Choice.configure_) { report(Token, "cancelled"); emit ConfigureRequested(); return true; }
    if (Choice.handler_.empty() || cancelled_.contains(Token)) { report(Token, "cancelled"); return true; }
    std::vector<hc::plugins::HostedViewerSession::DocumentInput> Inputs;
    HandlerSelection ChosenSelection;
    for (const auto Index : Choice.indexes_) {
        Inputs.push_back(Request.documents_.at(Index));
        ChosenSelection.names_.push_back(Selection.names_.at(Index));
        ChosenSelection.textContent_.push_back(Index < Selection.textContent_.size() && Selection.textContent_[Index]);
        if (const auto At = Selection.handlerMatches_.find(Choice.handler_); At != Selection.handlerMatches_.end())
            ChosenSelection.handlerMatches_[Choice.handler_].push_back(Index < At->second.size() && At->second[Index]);
    }
    Request.documents_ = std::move(Inputs);
    if (!Request.stillCurrent_()) { report(Token, "rejected", "document-context-changed"); return true; }
    prepare(Token, std::move(Request), Catalog.targets_.at(Choice.handler_), std::move(ChosenSelection)); return true;
}
QStringList DocumentOpenController::Arguments(const QString& Template, const QStringList& Paths) {
    auto Result = QProcess::splitCommand(Template); bool Found = false;
    QStringList Expanded;
    for (auto Argument : Result) {
        if (Argument == "%FILES%") { for (const auto& File : Paths) Expanded.append(QDir::toNativeSeparators(File)); Found = true; continue; }
        if (Argument.contains("%FILE%") && !Paths.empty()) { Argument.replace("%FILE%", QDir::toNativeSeparators(Paths.front())); Found = true; }
        Expanded.append(Argument);
    }
    if (!Found) for (const auto& File : Paths) Expanded.append(QDir::toNativeSeparators(File)); return Expanded;
}
void DocumentOpenController::prepare(const QString& Token, DocumentOpenRequest Request, DocumentHandlerTarget Target, HandlerSelection Selection) {
    const bool NonNative = std::any_of(Request.documents_.begin(), Request.documents_.end(), [](const auto& Input) {
        const auto Kind = Input.source_->Describe().locator_.kind_; return Kind != LocatorKind::Local && Kind != LocatorKind::Smb;
    });
    // Non-native HyperEditor inputs use the existing product broker so Save
    // targets the original provider, never the materialized local copy.
    const bool Plugin = Target.descriptor_.kind_ == HandlerKind::Plugin || (Target.productEditor_ && NonNative);
    const auto Stop = std::make_shared<std::stop_source>(); pending_.insert(Token, Stop);
    auto* Watcher = new QFutureWatcher<PreparedOpen>(this);
    connect(Watcher, &QFutureWatcher<PreparedOpen>::finished, this, [this, Watcher, Token, Request, Target, Plugin, NonNative, Selection] {
        auto Prepared = Watcher->result(); Watcher->deleteLater(); pending_.remove(Token);
        if (cancelled_.contains(Token)) { report(Token, "cancelled"); return; }
        if (!Prepared.error_.isEmpty()) { report(Token, "failed", Prepared.error_); return; }
        if (!main_ || !Request.stillCurrent_()) { report(Token, "rejected", "document-context-changed"); return; }
        const auto Fresh = catalog_(); const auto At = Fresh.targets_.find(Target.descriptor_.stableId_);
        std::vector<std::string> Names; for (const auto& Input : Request.documents_) Names.push_back(Input.source_->Describe().name_);
        const auto Detected = Selection.handlerMatches_.find(Target.descriptor_.stableId_);
        if (At == Fresh.targets_.end() || !DocumentHandlerResolver::Supports(At->second.descriptor_, Request.intent_, Names, Selection.textContent_,
                Detected == Selection.handlerMatches_.end() ? std::vector<bool>{} : Detected->second)
            || At->second.executable_ != Target.executable_ || At->second.arguments_ != Target.arguments_ || At->second.rhwpEditor_ != Target.rhwpEditor_
            || At->second.plugin_.binaryPath_ != Target.plugin_.binaryPath_
            || At->second.plugin_.applicationRule_ != Target.plugin_.applicationRule_
            || At->second.plugin_.detectionOverride_ != Target.plugin_.detectionOverride_
            || At->second.plugin_.approval_.reviewed_ != Target.plugin_.approval_.reviewed_) {
            report(Token, "rejected", "document-handler-changed"); return;
        }
        if (Plugin) {
            report(Token, "preparing");
            const auto Completion = [Guard = QPointer<DocumentOpenController>(this), Token](bool Ready, const QString& Error) {
                if (Guard) Guard->report(Token, Ready ? "ready" : "failed", Error);
            };
            if (!launch_ || !launch_(Target, Request.documents_, Completion)) report(Token, "failed", "document-plugin-open"); return;
        }
        auto Shared = std::make_shared<PreparedOpen>(std::move(Prepared));
        const auto StartsLeft = std::make_shared<qsizetype>(Target.descriptor_.delivery_ == FileDelivery::OnePerProcess ? Shared->paths_.size() : 1);
        const auto StartFailed = std::make_shared<bool>(false);
        const auto Start = [this, Shared, Target, Token, StartsLeft, StartFailed, NonNative](const QStringList& Files) {
            auto* Process = new QProcess(this); const bool Leased = !Shared->leases_.empty(); Process->setProperty("hc.nativeDocument", !Leased); if (Leased) ++leasedProcesses_;
            const auto Released = std::make_shared<bool>(false);
            const auto Release = [this, Process, Shared, Leased, Released] {
                if (*Released) return; *Released = true; if (Leased) --leasedProcesses_; Process->deleteLater();
            };
            connect(Process, &QProcess::errorOccurred, this, [this, Token, Release, StartFailed](QProcess::ProcessError Error) { if (Error == QProcess::FailedToStart) { *StartFailed = true; report(Token, "failed", "document-executable-start-failed"); Release(); } });
            connect(Process, &QProcess::finished, this, [Release](int, QProcess::ExitStatus) { Release(); });
            connect(Process, &QProcess::started, this, [this, Token, StartsLeft, StartFailed] { if (--*StartsLeft == 0 && !*StartFailed) report(Token, "started"); });
            auto Args = Arguments(Target.arguments_, Files);
            if (Target.rhwpEditor_ && NonNative) Args.replaceInStrings("--rhwp-edit", "--rhwp-edit-copy");
            if (Target.productEditor_) {
                QStringList Prefix{"--config-dir", hc::settings::DocumentPaths().directory_};
                if(std::any_of(Files.begin(),Files.end(),[](const QString& File){return QFileInfo(File).suffix().compare("epub",Qt::CaseInsensitive)==0;}))
                    Prefix << "--document-appearance" << QString::fromLatin1(QJsonDocument(epubui::Appearance()).toJson(QJsonDocument::Compact).toBase64());
                Args = Prefix + QStringList{"--"} + Args;
            }
            Process->start(Target.executable_, Args);
        };
        if (Target.descriptor_.delivery_ == FileDelivery::OnePerProcess) {
            for (qsizetype I = 0; I < Shared->paths_.size(); ++I) Start({Shared->paths_[I]});
        } else Start(Shared->paths_);
    });
    Watcher->setFuture(QtConcurrent::run([Request, Plugin, Stop] {
        PreparedOpen Result; OperationContext Context; Context.stopToken_ = Stop->get_token(); Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        for (const auto& Input : Request.documents_) {
            auto Verified = Input.source_->Verify(Context).get(); if (!Verified.HasValue()) { Result.error_ = QString::fromStdString(Verified.Failure().code_); return Result; }
            if (Plugin) continue;
            const auto Entry = Input.source_->Describe();
            if (Entry.locator_.kind_ == LocatorKind::Local || Entry.locator_.kind_ == LocatorKind::Smb) { Result.paths_.append(QString::fromStdString(Entry.locator_.root_)); continue; }
            if (!Result.materializer_) {
                auto Factory = MakeNativeMaterializer(QDir::tempPath().toStdString());
                if (!Factory.HasValue()) { Result.error_ = QString::fromStdString(Factory.Failure().code_); return Result; } Result.materializer_ = Factory.Value();
            }
            auto Lease = Result.materializer_->Materialize({Input.source_}, Context).get();
            if (!Lease.HasValue()) { Result.error_ = QString::fromStdString(Lease.Failure().code_); return Result; }
            Result.leases_.push_back(Lease.Value()); Result.paths_.append(QString::fromStdString(Lease.Value()->LocalPath()));
        }
        return Result;
    }));
}
bool DocumentOpenController::eventFilter(QObject* Object, QEvent* Event) {
    if (Object == main_ && Event->type() == QEvent::Close && (leasedProcesses_ || !pending_.empty())) {
        static_cast<QCloseEvent*>(Event)->ignore(); emit Finished({}, "rejected", leasedProcesses_ ? "외부 프로그램이 임시 파일을 사용 중입니다. 해당 프로그램을 닫은 후 종료할 수 있습니다." : "문서를 준비 중입니다. 준비가 끝난 후 종료할 수 있습니다."); return true;
    }
    if (Object == main_ && Event->type() == QEvent::Close) {
        // Native EXEs use the original path and remain independent after HC
        // closes. Their self-owned QProcess retires when the child exits; no
        // destructor on HC's window kills the separately opened editor.
        for (auto* Process : findChildren<QProcess*>()) if (Process->property("hc.nativeDocument").toBool()) {
            Process->setParent(nullptr);
            connect(Process, &QProcess::finished, Process, [Process](int, QProcess::ExitStatus) { Process->deleteLater(); });
            connect(Process, &QProcess::errorOccurred, Process, [Process](QProcess::ProcessError Error) { if (Error == QProcess::FailedToStart) Process->deleteLater(); });
        }
    }
    return QObject::eventFilter(Object, Event);
}
}
