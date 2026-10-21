#include "hwpStartupTrace.hpp"
#include "hwpOpenWindow.hpp"
#include "documentFocusRestore.hpp"
#include "hwpInputSender.hpp"
#include "hwpInputInspection.hpp"
#include "UniqueLibs/nativeWritableStorage.hpp"
#include "commonLibs/document/documentSaveCoordinator.hpp"
#include "commonLibs/layout/productLayout.hpp"
#include <QCryptographicHash>
#include <QFile>
#include <QFileDialog>
#include <QPrintDialog>
#include <QPrinter>
#include <QPainter>
#include <QImage>
#include <QPageSize>
#include <QAction>
#include <QKeySequence>
#include <atomic>
#include <QMessageBox>
#include "UniqueLibs/nativeMaterializer.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include <QtConcurrentRun>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QVBoxLayout>
#include <QUuid>
#include <QTimer>
#include <QPointer>
#include <thread>
#include <utility>
namespace hc::document
{
    WebProcessRuntime MakeProductHwpRuntime(const QString& ApplicationDirectory,
        QByteArray ApprovedInventorySha256)
    {
        WebProcessRuntime Runtime;
        // Keep the optional engaged even for an empty policy: product callers
        // must never fall back to diagnostic file-map admission.
        Runtime.approvedInventorySha256_ = std::move(ApprovedInventorySha256);
        if (ApplicationDirectory.isEmpty() || !QFileInfo(ApplicationDirectory).isAbsolute()) return Runtime;
        // HyperEditor always runs from ROOT; approval locks and hashes the runtime before launch.
        const auto Root = QDir::cleanPath(QDir::fromNativeSeparators(ApplicationDirectory));
        Runtime.root_ = QDir::cleanPath(QDir(Root).filePath(hc::layout::RhwpRuntime));
        return Runtime;
    }
    struct HwpOpenWindow::PrintJob {
        std::shared_ptr<QPrinter> printer_=std::make_shared<QPrinter>(QPrinter::HighResolution);
        std::atomic<bool> cancel_=false;
        QString failure_;
    };
    struct HwpOpenWindow::SaveDestination
    {
        QString path_, error_;
        storage::DestinationExpectation expected_;
        QByteArray hash_;
    };
    struct HwpOpenWindow::Prepared
    {
        std::shared_ptr<storage::IMaterializer> materializer_;
        SourceIdentity source_;
        QString error_;
    };
    HwpOpenWindow::HwpOpenWindow(QString Path, QString CacheRoot, WebProcessRuntime Runtime, BackendFactory Factory, bool Editable, bool OriginalWritable, PrintDialogFactory PrintDialog)
        : path_(std::move(Path)), cacheRoot_(std::move(CacheRoot)), runtime_(std::move(Runtime)), factory_(std::move(Factory)), editable_(Editable), originalWritable_(Editable && OriginalWritable), printDialogFactory_(std::move(PrintDialog))
    {
        // Install the retirement filter before any owner decision filter.
        HwpProcessBackend::InitializeLifecycle();
        if(!editable_)setFocusPolicy(Qt::StrongFocus);
        if(editable_)new DocumentFocusRestore(this,[this]()->QWidget*{return document_&&!closing_?document_:nullptr;},[this]{
            if(document_)if(auto* Backend=dynamic_cast<HwpProcessBackend*>(document_->focusProxy()))Backend->FocusInput();
        });
        setWindowTitle(QFileInfo(path_).fileName() + tr(" — 불러오는 중")); resize(1000, 760);
        auto* Layout = new QVBoxLayout(this); Layout->setContentsMargins(0, 0, 0, 0);
        status_ = new QLabel(tr("문서를 준비하고 있습니다…"), this);
        status_->setAlignment(Qt::AlignCenter); status_->setWordWrap(true); Layout->addWidget(status_);
        connect(&preparation_, &QFutureWatcherBase::finished, this, &HwpOpenWindow::prepared);
        connect(&cleanup_, &QFutureWatcherBase::finished, this, [this] {
            if (!cleaning_ || cleanup_.future().resultCount() == 0) return;
            const auto Error = cleanup_.result(); cleaning_ = false; cleanup_.setFuture({});
            const QPointer<HwpOpenWindow> Guard(this);
            if (!Error.isEmpty()) fail(Error);
            if (Guard && closing_) finishClose();
        });
        connect(this, &HwpOpenWindow::StateChanged, this, [this] {
            const auto State = document_ ? document_->Snapshot() : DocumentSnapshot{};
            const auto Caption = !error_.isEmpty() || (document_ && State.lifecycle_ == DocumentLifecycle::Failed)
                ? tr(" — 열기 실패") : !document_ || State.lifecycle_ != DocumentLifecycle::Ready
                ? tr(" — 불러오는 중") : editable_ ? tr(" — 한글 문서 편집") : tr(" — 한글 문서 보기");
            setWindowTitle(QFileInfo(path_).fileName() + (document_ && State.dirty_ ? " *" : "") + Caption);
            if(document_ && State.lifecycle_ == DocumentLifecycle::Ready) {
                startup::Data().EndNamed("editor.publishReady");startup::Data().EndNamed("editor.entryToTitle");
            }
        });
        connect(&saving_, &QFutureWatcherBase::finished, this, [this] {
            if (!saveTicket_ || saving_.future().resultCount() == 0) return;
            const auto Ticket = *saveTicket_; auto Result = saving_.result(); const auto Committed = Result.committed_;
            saveTicket_.reset(); saving_.setFuture({});
            if (Committed) { path_ = QString::fromStdString(Ticket.destination_); originalWritable_ = true; }
            const QPointer<HwpOpenWindow> Guard(this);
            if (document_) document_->CompleteSave(Ticket, std::move(Result));
            if (!Guard) return;
            if (document_ && closeFrozen_) { closeFrozen_ = false; document_->ResumeEditing(); }
            if (!Guard) return;
            if (!Committed) {
                saveThenClose_ = false; quitPending_ = false; QMessageBox::warning(this, tr("저장 실패"), tr("저장하지 못했습니다. 원본 변경 또는 파일 접근 상태를 확인해 주세요. 편집 내용은 유지됩니다."));
            }
        });
        connect(&destinationPreparation_, &QFutureWatcherBase::finished, this, [this] {
            if (!preparingDestination_) return;
            const QPointer<HwpOpenWindow> Guard(this);
            preparingDestination_ = false; saveDestination_ = destinationPreparation_.result();
            destinationPreparation_.setFuture({});
            if (!saveDestination_->error_.isEmpty()) {
                saveDestination_.reset(); saveThenClose_ = false; quitPending_ = false;
                if (document_ && closeFrozen_) { closeFrozen_ = false; document_->ResumeEditing(); }
                if (!Guard) return;
                QMessageBox::warning(this, tr("저장 위치 확인 실패"), tr("저장 대상의 상태를 확인하지 못했습니다. 파일 접근 권한과 변경 여부를 확인해 주세요."));
            } else {
                const bool Started = document_ && document_->Save({SaveDisposition::SaveAs, saveDestination_->path_.toUtf8().toStdString()},
                    QFileInfo(saveDestination_->path_).suffix().toLower());
                if (!Guard) return;
                if (!Started) {
                    saveDestination_.reset(); saveThenClose_ = false; quitPending_ = false;
                    if (document_ && closeFrozen_) { closeFrozen_ = false; document_->ResumeEditing(); }
                }
            }
            if (Guard) emit StateChanged();
        });
        connect(&printing_, &QFutureWatcherBase::finished, this, [this] {
            if(printing_.future().resultCount()) {auto Result=printing_.result();printing_.setFuture({});finishPrint(std::move(Result));}
        });
        auto* PrintAction=new QAction(tr("인쇄"),this);PrintAction->setObjectName("rhwp.nativePrint");
        // Web content owns Ctrl+P when embedded; native action serves read-only/native focus.
        PrintAction->setShortcut(QKeySequence::Print);PrintAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        addAction(PrintAction);connect(PrintAction,&QAction::triggered,this,&HwpOpenWindow::Print);
        setContextMenuPolicy(Qt::ActionsContextMenu);
        qApp->installEventFilter(this);
    }
    HwpOpenWindow::~HwpOpenWindow()
    {
        qApp->removeEventFilter(this); stop_.request_stop();
        if(printJob_)printJob_->cancel_=true;
        // Drop all GUI-owned source references while resources_ still retains
        // the materializer. Its final destruction belongs to the worker below.
        delete document_; document_ = nullptr;
        // Normal ownership reaches Closed first. Unexpected owner destruction
        // still retains an acquisition result off the GUI thread through cleanup.
        auto Future = preparation_.future(); auto Resources = std::move(resources_);
        if (Future.isValid() || Resources) std::thread([Future, Resources = std::move(Resources)]() mutable {
            if (Future.isValid()) { Future.waitForFinished(); if (Future.resultCount()) Resources = Future.result(); }
            if (Resources) {
                Resources->source_ = {};
                if (Resources->materializer_) Resources->materializer_->DrainCleanup();
                Resources.reset();
            }
        }).detach();
    }
    void HwpOpenWindow::fail(QString Error)
    {
        error_ = std::move(Error);
        status_->setText(error_ == "rhwp-runtime-policy-unavailable"
            ? tr("현재 설치에서는 한글 문서 보기를 사용할 수 없습니다.")
            : tr("한글 문서를 준비하지 못했습니다. 파일 위치와 접근 권한을 확인해 주세요."));
        status_->show(); emit StateChanged();
    }
    bool HwpOpenWindow::SetRecovery(RecoverySnapshot Record)
    {
        // Recovery compatibility is pinned to the complete qualified runtime,
        // including its embedded bundle. It never grants original-file writes.
        if (opened_ || closing_ || !editable_ || recovery_ || !runtime_.approvedInventorySha256_
            || Record.bundleSha256_ != runtime_.approvedInventorySha256_->toStdString()
            || !Record.document_.dirty_ || Record.format_ != QFileInfo(path_).suffix().toLower().toStdString()) return false;
        originalWritable_ = false; recovery_ = std::move(Record); return true;
    }
    void HwpOpenWindow::Open()
    {
        if (opened_ || closing_) return; opened_ = true;
        const auto Pin = runtime_.approvedInventorySha256_.value_or(QByteArray{});
        if (!runtime_.approvedInventorySha256_ || !runtime_.sha256_.isEmpty()
            || Pin.size() != 64 || QByteArray::fromHex(Pin).toHex() != Pin) {
            fail("rhwp-runtime-policy-unavailable"); return;
        }
        startup::Data().Begin("editor.sourcePreparation");
        preparing_ = true;
        const auto Path = path_, Cache = cacheRoot_; const auto Stop = stop_.get_token();
        const auto Editable = editable_, OriginalWritable = originalWritable_;
        preparation_.setFuture(QtConcurrent::run([Path, Cache, Stop, Editable, OriginalWritable] {
            using namespace storage;
            auto Out = std::make_shared<Prepared>(); OperationContext Context; Context.stopToken_ = Stop;
            const auto Fail = [&](QString Code) { Out->error_ = std::move(Code); return Out; };
            if (auto Error = Context.Check({"rhwp"})) return Fail(QString::fromStdString(Error->code_));
            if (!QFileInfo(Path).isAbsolute() || !QFileInfo(Cache).isAbsolute()) return Fail("rhwp-open-absolute-path");
            auto Provider = MakeNativeReadProvider(LocatorKind::Local);
            auto Entry = Provider->Resolve({LocatorKind::Local, Path.toUtf8().toStdString()}, Context).get();
            if (!Entry.HasValue()) return Fail(QString::fromStdString(Entry.Failure().code_));
            if (!QDir().mkpath(Cache)) return Fail("rhwp-cache-directory");
            MaterializationLimits Limits;
            if(Editable){Limits.maxFileBytes_=64ULL*1024*1024;Limits.maxTotalBytes_=256ULL*1024*1024;Limits.maxActiveFiles_=4;}
            else Limits.maxActiveFiles_=std::numeric_limits<std::size_t>::max();
            auto Store = MakeNativeMaterializer(Cache.toUtf8().toStdString(), Limits);
            if (!Store.HasValue()) return Fail(QString::fromStdString(Store.Failure().code_));
            Out->materializer_ = std::move(Store.Value()); MaterializationRequest Request;
            Request.source_ = std::make_shared<ProviderContentSource>(Provider, Entry.Value());
            auto Source = PrepareHwpSource(*Out->materializer_, std::move(Request), Context);
            if (!Source.HasValue()) return Fail(QString::fromStdString(Source.Failure().code_));
            Out->source_ = std::move(Source.Value());
            if (Editable) {
                startup::Scope InspectionSpan("source.editInspection");
                QFile File(QString::fromStdString(Out->source_.localPath_));
                if (!File.open(QIODevice::ReadOnly)) return Fail("rhwp-edit-input-open");
                const auto Bytes = File.read(64 * 1024 * 1024 + 1);
                const auto Inspection = InspectHwpInput(Bytes, QFileInfo(Path).suffix().toLower());
                if (!File.atEnd() || File.error() != QFileDevice::NoError || !Inspection.accepted_ || Inspection.restricted_)
                    return Fail("rhwp-edit-input-denied");
                Out->source_.originalWritable_ = OriginalWritable;
            }
            return Out;
        }));
        emit StateChanged();
    }
    void HwpOpenWindow::prepared()
    {
        if (!preparing_ || preparation_.future().resultCount() == 0) return;
        startup::Data().EndNamed("editor.sourcePreparation");
        resources_ = preparation_.result(); preparing_ = false; preparation_.setFuture({});
        if (closing_) { cleanup(); return; }
        const QPointer<HwpOpenWindow> Guard(this);
        if (!resources_->error_.isEmpty()) { fail(resources_->error_); if (Guard) cleanup(); return; }
        auto Backend = factory_ ? factory_() : std::make_unique<HwpProcessBackend>(runtime_, editable_?WebProcessLimits{8,2048ULL*1024*1024,10000}:WebProcessLimits{0,0,10000}, 30000, editable_);
        if (!Guard) return;
        if (closing_) { cleanup(); return; }
        if (!Backend) { fail("rhwp-backend-unavailable"); if (Guard) cleanup(); return; }
        const auto Id = recovery_ ? recovery_->document_.id_ : QUuid::createUuid().toString(QUuid::Id128).toStdString();
        document_ = new HwpSdiWindow(Id, resources_->source_, std::move(Backend), editable_);
        if (recovery_ && !document_->SetRecovery(std::move(*recovery_), *runtime_.approvedInventorySha256_, QFileInfo(path_).suffix().toLower())) {
            recovery_.reset(); delete document_; document_ = nullptr;
            fail("rhwp-recovery-incompatible"); if (Guard) cleanup(); return;
        }
        recovery_.reset();
        const bool HadFocus=hasFocus();
        document_->setParent(this); layout()->addWidget(document_); status_->hide(); document_->show();
        if(!editable_){setFocusProxy(document_);if(HadFocus)document_->setFocus(Qt::OtherFocusReason);}
        connect(document_, &HwpSdiWindow::StateChanged, this, &HwpOpenWindow::StateChanged);
        connect(document_, &HwpSdiWindow::Closed, this, [this] {
            closing_ = true; stop_.request_stop();
            // Queued destruction avoids deleting a sender inside its close stack.
            QTimer::singleShot(0, this, [this] { delete document_; document_ = nullptr; cleanup(); });
        });
        connect(document_, &HwpSdiWindow::ControlTimeout, this, &HwpOpenWindow::RequestClose, Qt::QueuedConnection);
        connect(document_, &HwpSdiWindow::CloseSynchronizationFailed, this, [this] { closeSyncPending_ = false; quitPending_ = false; });
        connect(document_, &HwpSdiWindow::StateChanged, this, [this] {
            if ((saveThenClose_ || quitPending_) && document_ && !saveTicket_ && !closeSyncPending_ && !closePrompt_ && !document_->IsBusy()) {
                saveThenClose_ = false; QTimer::singleShot(0, this, &HwpOpenWindow::RequestClose);
            }
        }, Qt::QueuedConnection);
        connect(document_, &HwpSdiWindow::CloseSynchronized, this, [this] {
            closeSyncPending_ = false; closeSynchronized_ = true; closeFrozen_ = true; RequestClose();
        });
        connect(document_, &HwpSdiWindow::PrintRequested,this,&HwpOpenWindow::Print);
        connect(document_, &HwpSdiWindow::PrintFailed,this,[this](const QString& Code) {
            if(!printJob_)return;
            printJob_->failure_=Code;printJob_->cancel_=true;
            if(!printing_.isRunning() && (!document_ || !document_->IsPrinting()))
                finishPrint(storage::Error{storage::ErrorCategory::InvalidRequest,Code.toStdString()});
        });
        connect(document_, &HwpSdiWindow::StateChanged,this,[this] {
            if(printJob_&&!printing_.isRunning()&&!printDialog_&&document_&&!document_->IsPrinting())
                finishPrint(storage::Error{storage::ErrorCategory::Cancelled,"rhwp-print-cancelled"});
        },Qt::QueuedConnection);
        connect(document_, &HwpSdiWindow::PrintPrepared,this,[this](HwpPrintFrames Frames) {
            if(!printJob_||printing_.isRunning())return;
            const auto Job=printJob_;
            printing_.setFuture(QtConcurrent::run([Job,Frames=std::move(Frames)] {
                using namespace storage;
                const auto Fail=[](const char* Code)->Result<Unit>{return storage::Error{ErrorCategory::InvalidRequest,Code};};
                std::vector<int> Pages;for(const auto& Page:Frames.pages_)Pages.push_back(Page.index_);
                // Revalidate and build a bounded immutable PDF before opening a printer painter.
                auto Verified=BuildHwpPdf(Frames.ticket_,Pages,Frames);if(!Verified.HasValue())return Result<Unit>(Verified.Failure());
                if(Job->cancel_.load())return Fail("rhwp-print-cancelled");
                auto& Printer=*Job->printer_;QPainter Painter;
                if(!Painter.begin(&Printer))return Fail("rhwp-print-device");
                for(size_t I=0;I<Frames.pages_.size();++I) {
                    if(Job->cancel_.load()){Printer.abort();Painter.end();return Fail("rhwp-print-cancelled");}
                    if(I&&!Printer.newPage()){Painter.end();return Fail("rhwp-print-new-page");}
                    const auto Image=QImage::fromData(Frames.pages_[I].png_,"PNG");
                    const auto Rect=Printer.pageLayout().paintRectPixels(Printer.resolution());
                    const auto Size=Frames.pages_[I].paperPixels_.scaled(Rect.size(),Qt::KeepAspectRatio);
                    const QRectF Target(Rect.x()+(Rect.width()-Size.width())/2,Rect.y()+(Rect.height()-Size.height())/2,Size.width(),Size.height());
                    Painter.drawImage(Target,Image);
                }
                if(!Painter.end()||Printer.printerState()==QPrinter::Error||Printer.printerState()==QPrinter::Aborted)return Fail("rhwp-print-device");
                return Result<Unit>(Unit{});
            }));
        });
        connect(document_, &HwpSdiWindow::SaveRequested, this, &HwpOpenWindow::Save);
        connect(document_, &HwpSdiWindow::SaveFailed, this, [this](const QString&) {
            const QPointer<HwpOpenWindow> Guard(this);
            saveThenClose_ = false; quitPending_ = false; saveDestination_.reset();
            if (document_ && closeFrozen_) { closeFrozen_ = false; document_->ResumeEditing(); }
            if (!Guard) return;
            QMessageBox::warning(this, tr("저장 실패"), tr("문서를 안전하게 내보내지 못했습니다. 편집 내용은 유지됩니다. 내용 손실이 예상되거나 저장 중 문서가 변경된 경우 다시 확인해 주세요."));
        });
        connect(document_, &HwpSdiWindow::SavePrepared, this, [this](SaveTicket Ticket) {
            if (saveTicket_) return;
            saveTicket_ = Ticket;
            const auto DestinationState = std::exchange(saveDestination_, {});
            saving_.setFuture(QtConcurrent::run([Ticket, DestinationState] {
                using namespace storage;
                DocumentSaveResult Result;
                const auto Fail = [&](storage::Error Failure) { Result.error_ = std::move(Failure); return Result; };
                const auto Destination = QString::fromStdString(Ticket.destination_);
                auto Reader = MakeNativeReadProvider(LocatorKind::Local);
                auto Writer = MakeNativeWritableStorage(LocatorKind::Local, Reader);
                DestinationExpectation Expected;
                if (Ticket.disposition_ == SaveDisposition::SaveAs) {
                    if (!DestinationState || DestinationState->path_ != Destination)
                        return Fail({ErrorCategory::AccessDenied, "rhwp-save-target-not-captured"});
                    Expected = DestinationState->expected_;
                    if (Expected.exists_) {
                        QFile Existing(Destination);
                        if (!Existing.open(QIODevice::ReadOnly)) return Fail({ErrorCategory::AccessDenied, "rhwp-save-read"});
                        const auto Bytes = Existing.read(64 * 1024 * 1024 + 1);
                        if (!Existing.atEnd() || Existing.error() != QFileDevice::NoError
                            || QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256) != DestinationState->hash_)
                            return Fail({ErrorCategory::StaleRevision, "rhwp-save-target-content-changed"});
                    }
                }
                if (Ticket.disposition_ == SaveDisposition::Original) {
                    if (!Ticket.source_.originalWritable_ || !Ticket.source_.entry_)
                        return Fail({ErrorCategory::AccessDenied, "rhwp-original-write-denied"});
                    Expected = {true, Ticket.source_.entry_->id_.revision_};
                    QFile Existing(Destination);
                    if (!Existing.open(QIODevice::ReadOnly)) return Fail({ErrorCategory::AccessDenied, "rhwp-save-read"});
                    const auto Bytes = Existing.read(64 * 1024 * 1024 + 1);
                    if (!Existing.atEnd() || Existing.error() != QFileDevice::NoError
                        || QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256).toHex().toStdString() != Ticket.source_.inputSha256_)
                        return Fail({ErrorCategory::StaleRevision, "rhwp-original-content-changed"});
                }
                const Locator Target{LocatorKind::Local, Destination.toUtf8().toStdString()};
                const auto Saved = SaveDocumentBytes(*Writer, Target, Expected,
                    std::as_bytes(std::span(Ticket.text_.data(), Ticket.text_.size())), {});
                if (!Saved.HasValue()) return Fail(Saved.Failure());
                Result.committed_ = true; Result.cleanupErrors_ = Saved.Value().cleanupErrors_;
                auto Fresh = Reader->Resolve(Target, {}).get();
                if (Fresh.HasValue()) {
                    auto Source = Ticket.source_; Source.entry_ = Fresh.Value(); Source.originalWritable_ = true;
                    Source.inputSha256_ = QCryptographicHash::hash(QByteArray::fromStdString(Ticket.text_), QCryptographicHash::Sha256).toHex().toStdString();
                    Result.source_ = std::move(Source);
                } else Result.cleanupErrors_.push_back(Fresh.Failure());
                return Result;
            }));
        });
        document_->Open(); if (Guard) emit StateChanged();
    }
    void HwpOpenWindow::cleanup()
    {
        if (cleaning_ || preparing_ || document_) return;
        if (!resources_) { if (closing_) finishClose(); return; }
        cleaning_ = true; auto Resources = std::move(resources_);
        cleanup_.setFuture(QtConcurrent::run([Resources = std::move(Resources)]() mutable {
            Resources->source_ = {}; QString Error;
            if (Resources->materializer_) {
                const auto Result = Resources->materializer_->DrainCleanup();
                if (!Result.errors_.empty()) Error = QString::fromStdString(Result.errors_.front().code_);
                else if (Result.busy_) Error = "rhwp-cleanup-busy";
                Resources->materializer_.reset();
            }
            Resources.reset(); return Error;
        }));
    }
    void HwpOpenWindow::Print()
    {
        if(printJob_||!document_||closing_||saveTicket_||preparingDestination_||document_->IsBusy()||!document_->Capabilities().print_)return;
        printJob_=std::make_shared<PrintJob>();const auto Job=printJob_;
        Job->printer_->setDocName(QFileInfo(path_).fileName());
        Job->printer_->setFullPage(true);
        Job->printer_->setPageSize(QPageSize(QPageSize::A4));
        const QPointer<HwpOpenWindow> Guard(this);
        bool Accepted=false;
        if(printDialogFactory_)Accepted=printDialogFactory_(*Job->printer_,this);
        else {
            QPrintDialog Dialog(Job->printer_.get(),this);printDialog_=&Dialog;Dialog.setWindowTitle(tr("한글 문서 인쇄"));
            Dialog.setOption(QAbstractPrintDialog::PrintPageRange,true);Dialog.setOption(QAbstractPrintDialog::PrintToFile,true);
            Dialog.setMinMax(1,std::numeric_limits<int>::max());Accepted=Dialog.exec()==QDialog::Accepted;
            if(!Guard)return;printDialog_.clear();
        }
        if(!Guard)return;
        if(!Accepted||Job->cancel_.load()) {finishPrint(storage::Error{storage::ErrorCategory::Cancelled,"rhwp-print-cancelled"});return;}
        std::vector<int> Pages;
        if(Job->printer_->printRange()==QPrinter::PageRange) {
            const auto First=Job->printer_->fromPage(),Last=Job->printer_->toPage();
            if(First<1||Last<First){finishPrint(storage::Error{storage::ErrorCategory::InvalidRequest,"rhwp-print-page-range"});return;}
            for(int I=First;I<=Last;++I)Pages.push_back(I-1);
        }
        if(!document_->Print(Pages)) {if(Guard)finishPrint(storage::Error{storage::ErrorCategory::InvalidRequest,"rhwp-print-unavailable"});}
    }
    void HwpOpenWindow::finishPrint(storage::Result<storage::Unit> Result)
    {
        if(!printJob_)return;
        if(!printJob_->failure_.isEmpty())Result=storage::Error{storage::ErrorCategory::InvalidRequest,printJob_->failure_.toStdString()};
        const bool Failed=!Result.HasValue() && Result.Failure().category_!=storage::ErrorCategory::Cancelled && (!printJob_->cancel_.load() || !printJob_->failure_.isEmpty());
        printJob_.reset();const QPointer<HwpOpenWindow> Guard(this);
        if(document_&&document_->IsPrinting())document_->CompletePrint(std::move(Result));
        if(!Guard)return;
        if(Failed)QMessageBox::warning(this,tr("인쇄 실패"),tr("문서를 인쇄하지 못했습니다. 편집 내용과 원본은 유지됩니다."));
        if(!Guard)return;
        emit StateChanged();
        if(Guard&&closeAfterPrint_){closeAfterPrint_=false;QTimer::singleShot(0,this,&HwpOpenWindow::RequestClose);}
    }
    void HwpOpenWindow::Save(bool SaveAs)
    {
        if (printJob_ || !document_ || saveTicket_ || preparingDestination_ || document_->IsBusy() || !editable_) return;
        const QPointer<HwpOpenWindow> Guard(this);
        auto Destination = path_; auto Disposition = SaveDisposition::Original;
        if (SaveAs || !originalWritable_) {
            // The native dialog runs a nested event loop. Gate queued menu and
            // shortcut requests before entering it, not after it returns.
            preparingDestination_ = true;
            Destination = QFileDialog::getSaveFileName(this, tr("다른 이름으로 저장"), path_, tr("한글 문서 (*.hwp *.hwpx)"));
            if (!Guard) return;
            if (Destination.isEmpty()) { preparingDestination_ = false; emit StateChanged();
                if (!Guard) return;
                saveThenClose_ = false; quitPending_ = false; closeFrozen_ = false; document_->ResumeEditing(); return; }
            preparingDestination_ = true;
            // Capture before export; the atomic writer rechecks this revision at
            // commit so a file created or replaced during export is not overwritten.
            destinationPreparation_.setFuture(QtConcurrent::run([Destination] {
                using namespace storage;
                auto Out = std::make_shared<SaveDestination>(); Out->path_ = Destination;
                auto Reader = MakeNativeReadProvider(LocatorKind::Local);
                const Locator Target{LocatorKind::Local, Destination.toUtf8().toStdString()};
                auto Before = Reader->Resolve(Target, {}).get();
                if (!Before.HasValue()) {
                    if (Before.Failure().category_ != ErrorCategory::NotFound) Out->error_ = "rhwp-save-target-read";
                    return Out;
                }
                Out->expected_ = {true, Before.Value().id_.revision_};
                QFile Existing(Destination);
                if (!Existing.open(QIODevice::ReadOnly)) { Out->error_ = "rhwp-save-target-open"; return Out; }
                const auto Bytes = Existing.read(64 * 1024 * 1024 + 1);
                if (!Existing.atEnd() || Existing.error() != QFileDevice::NoError) {
                    Out->error_ = "rhwp-save-target-budget"; return Out;
                }
                Out->hash_ = QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256);
                auto After = Reader->Resolve(Target, {}).get();
                if (!After.HasValue() || After.Value().id_ != Before.Value().id_) Out->error_ = "rhwp-save-target-changed";
                return Out;
            }));
            emit StateChanged(); return;
        }
        const auto Format = QFileInfo(Destination).suffix().toLower();
        const bool Started = document_->Save({Disposition, Destination.toUtf8().toStdString()}, Format);
        if (!Guard) return;
        if (!Started) {
            saveThenClose_ = false; quitPending_ = false;
            if (closeFrozen_) { closeFrozen_ = false; document_->ResumeEditing(); }
        }
    }
    void HwpOpenWindow::RequestClose()
    {
        if(printJob_ && (!document_ || !document_->ControlTimedOut())) {
            closeAfterPrint_=true;printJob_->cancel_=true;
            if(printDialog_)printDialog_->reject();
            else if(document_)document_->CancelPrint();
            return;
        }
        if (!closed_ && !closePrompt_ && document_ && document_->ControlTimedOut()) {
            const QPointer<HwpOpenWindow> Guard(this); closePrompt_ = true;
            const auto Answer = QMessageBox::warning(this, tr("편집기 응답 지연"),
                tr("편집 작업의 완료를 확인하지 못했습니다. 다시 기다리거나 변경 내용을 버리고 닫을 수 있습니다."),
                QMessageBox::Retry | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Retry);
            if (!Guard) return; closePrompt_ = false;
            if (Answer == QMessageBox::Retry) document_->RetryControl();
            else if (Answer == QMessageBox::Discard) { closing_ = true; document_->DiscardAndClose(); }
            else quitPending_ = false;
            return;
        }
        if (closed_ || closePrompt_ || saveTicket_ || preparingDestination_ || (document_ && document_->IsBusy() && document_->Snapshot().operation_ != DocumentOperation::Loading && !document_->IsClosing())) return;
        if (editable_ && document_ && document_->Snapshot().lifecycle_ == DocumentLifecycle::Ready && !closeSynchronized_) {
            if (!closeSyncPending_) closeSyncPending_ = document_->SynchronizeForClose();
            return;
        }
        closeSynchronized_ = false;
        auto Disposition = CloseDisposition::KeepChanges;
        if (document_ && document_->Snapshot().dirty_) {
            const QPointer<HwpOpenWindow> Guard(this); closePrompt_ = true;
            const auto Answer = QMessageBox::question(this, tr("변경 내용 저장"), tr("변경 내용을 저장하시겠습니까?"),
                QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
            if (!Guard) return; closePrompt_ = false;
            if (Answer == QMessageBox::Cancel) { quitPending_ = false; closeFrozen_ = false; document_->ResumeEditing(); return; }
            if (Answer == QMessageBox::Save) { saveThenClose_ = true; Save(!originalWritable_); return; }
            Disposition = CloseDisposition::Discard;
        }
        closing_ = true; stop_.request_stop();
        if (document_) document_->RequestClose(Disposition); else cleanup();
    }
    void HwpOpenWindow::finishClose()
    {
        if (closed_ || preparing_ || cleaning_ || document_) return;
        closed_ = true; const auto Quit = quitPending_; qApp->removeEventFilter(this);
        const QPointer<HwpOpenWindow> Guard(this); close(); if (Guard) emit Closed();
        // Replay after this owner drains. Other owners can defer the same Quit
        // in turn; the event reaches QApplication only after every owner retires.
        if (Quit) QTimer::singleShot(0, qApp, &QCoreApplication::quit);
    }
    void HwpOpenWindow::closeEvent(QCloseEvent* Event)
    {
        if (closed_) Event->accept();
        else {
            Event->ignore();
            // Immediate policy/preparation failure has no asynchronous work to
            // drain. Defer closing to avoid recursing into QWidget::close().
            QTimer::singleShot(0, this, &HwpOpenWindow::RequestClose);
        }
    }
    bool HwpOpenWindow::eventFilter(QObject* Object, QEvent* Event)
    {
        if (Object == qApp && Event->type() == QEvent::Quit && !closed_) {
            quitPending_ = true; Event->ignore(); RequestClose(); return true;
        }
        return QWidget::eventFilter(Object, Event);
    }
}
