#include "documentOpenController.hpp"
#include "commonLibs/document/epub/epubAppearance.hpp"
#include "documentHandlerMenu.hpp"
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
#include <algorithm>
namespace hc::document {
using namespace hc::storage;
namespace {
struct PreparedOpen {
    QString error_;
    QStringList paths_;
    std::vector<std::shared_ptr<IMaterializedFileLease>> leases_;
    std::shared_ptr<IMaterializer> materializer_;
};
bool LooksLikeLocalText(const storage::EntrySnapshot& Entry) {
    if (Entry.locator_.kind_ != LocatorKind::Local || Entry.directory_) return false;
    QFile File(QString::fromStdString(Entry.locator_.root_));
    if (!File.open(QIODevice::ReadOnly)) return false;
    auto Sample = File.read(64 * 1024);
    if (Sample.isEmpty() && File.size()) return false;
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
        const auto Ext = QFileInfo(QString::fromStdString(Entry.name_)).suffix().toLower().toStdString();
        const bool Known = std::find(DocumentExtensions.begin(), DocumentExtensions.end(), Ext) != DocumentExtensions.end();
        Selection.textContent_.push_back(!Known && LooksLikeLocalText(Entry));
    }
    auto Catalog = catalog_();
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
        if (At == Fresh.targets_.end() || !DocumentHandlerResolver::Supports(At->second.descriptor_, Request.intent_, Names, Selection.textContent_)
            || At->second.executable_ != Target.executable_ || At->second.arguments_ != Target.arguments_ || At->second.rhwpEditor_ != Target.rhwpEditor_
            || At->second.plugin_.binaryPath_ != Target.plugin_.binaryPath_
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
