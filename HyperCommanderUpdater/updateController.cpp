#include "stdafx.h"
#include "updateController.hpp"

#include "updateArchive.hpp"
#include "updateTransaction.hpp"
#include "UniqueLibs/settings/appSettings.hpp"
#include "UniqueLibs/solTypeDefs_Ver.hpp"
#include <QAction>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QMenu>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <QtConcurrent/QtConcurrentRun>

namespace hc::update {
namespace {
Path Native(const QString& Value) { return Path(Value.toStdWString()); }
QString Text(std::string_view Value) { return QString::fromUtf8(Value.data(), qsizetype(Value.size())); }
void Require(bool Value, const char* Error) { if (!Value) throw std::runtime_error(Error); }
QJsonObject Options() { return hc::settings::AppSettings().Read(hc::settings::Section::General).toObject()["update"].toObject(); }
bool SaveOption(const QString& Name, const QJsonValue& Value) {
    auto& Settings = hc::settings::AppSettings(); if (Settings.Editing() || !Settings.Writable()) return false;
    auto General = Settings.Read(hc::settings::Section::General).toObject(); auto Update = General["update"].toObject(); Update[Name] = Value; General["update"] = Update;
    const auto Previous = Settings.Object();
    if (Settings.Put(hc::settings::Section::General, General) && Settings.Commit()) return true;
    Settings.Replace(Previous); return false;
}
bool AllowedUrl(const QUrl& Url) {
    if (Url.scheme() != "https" || !Url.userInfo().isEmpty() || !Url.fragment().isEmpty() || (Url.port() != -1 && Url.port() != 443)) return false;
    if (Url.host() == "release-assets.githubusercontent.com") return Url.path().startsWith("/github-production-release-asset/");
    if (Url.host() != "github.com" || Url.hasQuery()) return false;
    static const QRegularExpression Path("^/jgh0721/hyperCommander/releases/(latest/download/update-manifest[.](json|cat)|download/v[0-9]+[.][0-9]+[.][0-9]+[.][0-9]+/(update-manifest[.](json|cat)|HyperCommander-[0-9]+[.][0-9]+[.][0-9]+[.][0-9]+-windows-x64-portable[.]zip))$");
    return Path.match(Url.path()).hasMatch();
}
void Eligible(const Manifest& Release, std::string_view Version) {
    const auto Current = ParseVersion(Version);
    Require(ParseVersion(Release.version_) > Current && Current >= ParseVersion(Release.minimum_), "update-version-order");
}
}
UpdateController::UpdateController(QWidget* Window, QMenu* Menu, std::function<bool()> CanRestart)
    : QObject(Window), window_(Window), canRestart_(std::move(CanRestart)), network_(new QNetworkAccessManager(this)), root_(QCoreApplication::applicationDirPath()), currentVersion_(nsHC::CLIENT_VERSION) {
    setObjectName("hc.update.controller");
    auto* CheckAction = Menu->addAction(tr("업데이트 확인…")); CheckAction->setObjectName("hc.update.check"); connect(CheckAction, &QAction::triggered, this, &UpdateController::Check);
    auto* Auto = Menu->addAction(tr("7일마다 업데이트 확인")); Auto->setCheckable(true); Auto->setChecked(Options()["automaticCheck"].toBool(true));
    connect(Auto, &QAction::toggled, this, [this, Auto](bool Enabled) {
        if (!SaveOption("automaticCheck", Enabled)) { Auto->blockSignals(true); Auto->setChecked(!Enabled); Auto->blockSignals(false); fail(tr("설정 편집을 마친 후 업데이트 설정을 저장하십시오.")); }
    });
    if (QStandardPaths::isTestModeEnabled()) return;
    QTimer::singleShot(1500, this, &UpdateController::restore);
    QTimer::singleShot(10000, this, [this] {
        if (busy_ || ready_ || !Options()["automaticCheck"].toBool(true) || hc::settings::AppSettings().Editing()) return;
        const auto Last = QDateTime::fromString(Options()["lastAttemptUtc"].toString(), Qt::ISODate);
        if (!Last.isValid() || Last.secsTo(QDateTime::currentDateTimeUtc()) >= 7 * 86400) check(true);
    });
    QTimer::singleShot(60000, this, [this] {
        const auto Root = root_;
        // Trust verification can use the system certificate cache; keep it off
        // the event thread along with inventory hashing.
        auto* Watcher = new QFutureWatcher<void>(this);
        connect(Watcher, &QFutureWatcher<void>::finished, Watcher, &QObject::deleteLater);
        Watcher->setFuture(QtConcurrent::run([Root] { try { AcknowledgeTransaction(Native(Root)); } catch (...) {} }));
    });
}
UpdateController::~UpdateController() { stop_.request_stop(); if (reply_) { reply_->disconnect(this); reply_->abort(); } }
void UpdateController::showProgress(const QString& Message) {
    if (!progress_) {
        progress_ = new QProgressDialog(window_); progress_->setWindowTitle(tr("HyperCommander 업데이트")); progress_->setCancelButtonText(tr("취소"));
        progress_->setMinimumDuration(0); progress_->setAutoClose(false); progress_->setAutoReset(false);
        connect(progress_, &QProgressDialog::canceled, this, &UpdateController::cancel);
    }
    progress_->setLabelText(Message); progress_->setRange(0, 0); if (!automatic_ || ready_) progress_->show();
}
void UpdateController::cancel() { stop_.request_stop(); if (reply_) reply_->abort(); }
void UpdateController::fail(const QString& Error) {
    busy_ = false; if (progress_) progress_->hide();
    // Preserve check/download failures as well as native helper failures, so a
    // dismissed dialog still leaves the reason available for diagnosis.
    try { DurableWrite(Native(root_) / L".update/last-error.json", "{\"error\":" + QuoteJson(Error.toUtf8().toStdString()) + "}"); } catch (...) {}
    if (!automatic_) QMessageBox::warning(window_, tr("업데이트"), tr("업데이트를 완료하지 못했습니다. 현재 설치 파일은 유지됩니다.\n%1").arg(Error));
}
void UpdateController::Check() {
    automatic_ = false;
    if (busy_) { if (progress_) progress_->show(); return; }
    if (ready_) { offer(); return; } check(false);
}
void UpdateController::run(std::function<Result(std::stop_token)> Work, std::function<void(Result)> Done) {
    busy_ = true; const auto Stop = stop_.get_token(); auto* Watcher = new QFutureWatcher<Result>(this);
    connect(Watcher, &QFutureWatcher<Result>::finished, this, [this, Watcher, Done = std::move(Done)] {
        auto Value = Watcher->result(); Watcher->deleteLater(); busy_ = false;
        if (stop_.stop_requested()) { if (progress_) progress_->hide(); return; }
        if (!Value.error_.isEmpty()) { fail(Value.error_); return; } Done(std::move(Value));
    });
    Watcher->setFuture(QtConcurrent::run([Work = std::move(Work), Stop] {
        try { return Work(Stop); } catch (const std::exception& Error) { return Result{{}, QString::fromUtf8(Error.what())}; }
    }));
}
void UpdateController::fetch(const QString& Address, const QString& Destination, std::uint64_t Limit, std::function<void()> Done, int Redirects) {
    if (stop_.stop_requested()) { busy_ = false; if (progress_) progress_->hide(); return; }
    const QUrl Url(Address);
    if (!AllowedUrl(Url) || Redirects > 5 || QDateTime::currentMSecsSinceEpoch() - transferStarted_ > 1800000) { fail("update-download-url-or-time-limit"); return; }
    auto File = std::make_shared<QSaveFile>(Destination); File->setDirectWriteFallback(false);
    try { CheckPlainPath(Native(Destination)); } catch (const std::exception& Error) { fail(Error.what()); return; }
    if (!File->open(QIODevice::WriteOnly)) { fail(File->errorString()); return; }
    QNetworkRequest Request(Url); Request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    Request.setRawHeader("User-Agent", QByteArray("HyperCommander/") + nsHC::CLIENT_VERSION); Request.setRawHeader("Accept-Encoding", "identity"); Request.setTransferTimeout(30000);
    auto* Reply = network_->get(Request); Reply->setReadBufferSize(65536); reply_ = Reply; auto Count = std::make_shared<std::uint64_t>(0); auto Error = std::make_shared<QString>();
    auto* Deadline = new QTimer(Reply); Deadline->setSingleShot(true);
    connect(Deadline, &QTimer::timeout, Reply, &QNetworkReply::abort); Deadline->start(int(1800000 - (QDateTime::currentMSecsSinceEpoch() - transferStarted_)));
    const auto Read = [Reply, File, Count, Error, Limit] {
        const auto Data = Reply->read(65536); if (Data.isEmpty()) return;
        if (std::uint64_t(Data.size()) > Limit - *Count) { *Error = "update-download-size-limit"; Reply->abort(); return; }
        *Count += Data.size();
        const auto Status = Reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (Status == 200 && File->write(Data) != Data.size()) { *Error = "update-download-write"; Reply->abort(); }
    };
    connect(Reply, &QNetworkReply::readyRead, this, [this, Reply, Read, Limit, Count] {
        while (Reply->bytesAvailable() > 0 && Reply->isOpen()) { const auto Before = Reply->bytesAvailable(); Read(); if (Reply->bytesAvailable() >= Before) break; }
        if (progress_ && Limit > 1048576) { progress_->setRange(0, 1000); progress_->setValue(int(*Count * 1000 / Limit)); }
    });
    connect(Reply, &QNetworkReply::finished, this, [this, Reply, File, Count, Error, Limit, Done = std::move(Done), Destination, Redirects, Read] {
        while (Reply->bytesAvailable() > 0 && Reply->isOpen()) { const auto Before = Reply->bytesAvailable(); Read(); if (Reply->bytesAvailable() >= Before) break; }
        Reply->deleteLater(); if (reply_ == Reply) reply_.clear();
        if (stop_.stop_requested()) { busy_ = false; if (progress_) progress_->hide(); return; }
        const auto Status = Reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (Status >= 300 && Status <= 399 && Error->isEmpty()) {
            const auto Next = Reply->url().resolved(Reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl()); File->cancelWriting();
            fetch(Next.toString(QUrl::FullyEncoded), Destination, Limit, Done, Redirects + 1); return;
        }
        const auto Declared = Reply->header(QNetworkRequest::ContentLengthHeader);
        if (!Error->isEmpty() || Reply->error() != QNetworkReply::NoError || Status != 200 || *Count == 0
            || (Declared.isValid() && Declared.toULongLong() != *Count) || !File->commit()) { fail(Error->isEmpty() ? "update-download-incomplete" : *Error); return; }
        Done();
    });
}
void UpdateController::check(bool Automatic) {
    automatic_ = Automatic; stop_ = std::stop_source{}; ready_ = false; showProgress(tr("새 버전과 서명을 확인하고 있습니다…"));
    SaveOption("lastAttemptUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    const auto Root = root_; const auto Current = currentVersion_; const auto Id = QUuid::createUuid().toString(QUuid::WithoutBraces); tx_ = QDir(Root).filePath(".update/" + Id); const auto Tx = tx_;
    run([Root, Tx, Current](std::stop_token) {
        CheckInstallRoot(Native(Root)); Require(StartupAllowed(Native(Root)) && !std::filesystem::exists(Native(Root) / L".update/active.json"), "update-recovery-or-health-check-pending");
        const auto Inventory = ParseInventory(ReadBounded(Native(Root) / L"product-inventory.json", 1048576));
        Require(Inventory.version_ == Current, "update-installed-version"); VerifyProductSignature(Native(Root) / L"HyperCommanderUpdater.exe");
        CheckPlainPath(Native(Tx)); std::filesystem::create_directories(Native(Tx)); return Result{};
    }, [this](Result) {
        busy_ = true; transferStarted_ = QDateTime::currentMSecsSinceEpoch();
        fetch("https://github.com/jgh0721/hyperCommander/releases/latest/download/update-manifest.json", QDir(tx_).filePath("update-manifest.json"), 262144, [this] {
            fetch("https://github.com/jgh0721/hyperCommander/releases/latest/download/update-manifest.cat", QDir(tx_).filePath("update-manifest.cat"), 1048576, [this] {
                const auto Tx = tx_;
                run([Tx](std::stop_token) { return Result{ParseManifest(AuthenticateManifest(Native(Tx) / L"update-manifest.cat", Native(Tx) / L"update-manifest.json")), {}}; }, [this](Result Value) {
                    release_ = std::move(Value.release_); if (progress_) progress_->hide();
                    const auto Current = ParseVersion(currentVersion_), Version = ParseVersion(release_.version_);
                    if (Version <= Current) { if (!automatic_) QMessageBox::information(window_, tr("업데이트"), tr("현재 버전 %1이 최신 버전입니다.").arg(nsHC::CLIENT_VERSION)); return; }
                    if (Current < ParseVersion(release_.minimum_)) { fail(tr("이 버전에서는 Portable ZIP을 직접 설치해야 합니다.")); return; }
                    if (automatic_ && Options()["skippedVersion"].toString() == Text(release_.version_)) return; offer();
                });
            });
        });
    });
}
void UpdateController::offer() {
    QMessageBox Box(QMessageBox::Information, tr("HyperCommander 업데이트"), ready_ ? tr("버전 %1의 검증과 설치 준비가 완료되었습니다.<br/>다른 HyperCommander·편집기 창을 닫고 재시작하십시오.").arg(Text(release_.version_))
        : tr("새 버전 %1을 사용할 수 있습니다.<br/>현재 버전: %2<br/>다운로드 크기: %3 MiB<br/><a href=\"%4\">변경 사항 보기</a>").arg(Text(release_.version_), nsHC::CLIENT_VERSION).arg(release_.zipBytes_ / 1048576).arg(Text(release_.notes_)), QMessageBox::NoButton, window_);
    Box.setTextFormat(Qt::RichText); Box.setTextInteractionFlags(Qt::TextBrowserInteraction);
    auto* Accept = Box.addButton(ready_ ? tr("업데이트 후 재시작") : tr("다운로드"), QMessageBox::AcceptRole);
    auto* Skip = ready_ ? nullptr : Box.addButton(tr("이 버전 건너뛰기"), QMessageBox::RejectRole); Box.addButton(tr("나중에"), QMessageBox::RejectRole); Box.exec();
    if (Box.clickedButton() == Accept) {
        automatic_ = false;
        if (!ready_) { download(); return; }
        try {
            if (!canRestart_() || OtherProductProcesses(Native(root_))) { QMessageBox::information(window_, tr("업데이트"), tr("파일 작업·명령 실행을 마치고 다른 HyperCommander·편집기·플러그인 창을 닫으십시오.")); return; }
        } catch (const std::exception& Error) { fail(Error.what()); return; }
        restartRequested_ = true; if (!window_->close()) restartRequested_ = false;
    } else if (Skip && Box.clickedButton() == Skip) { if (!SaveOption("skippedVersion", Text(release_.version_))) fail(tr("건너뛰기 설정을 저장하지 못했습니다.")); }
}
void UpdateController::download() {
    stop_ = std::stop_source{}; const auto Root = root_, Tx = tx_; const auto Release = release_; const auto Current = currentVersion_;
    showProgress(tr("Portable ZIP을 다운로드하고 있습니다…"));
    run([Root, Release, Current](std::stop_token) {
        Eligible(Release, Current); CheckInstallRoot(Native(Root)); Require(std::filesystem::space(Native(Root)).available >= Release.zipBytes_ + Release.unpackedBytes_ + 603979776, "update-disk-space"); return Result{};
    }, [this, Root, Tx, Release](Result) {
        busy_ = true; transferStarted_ = QDateTime::currentMSecsSinceEpoch();
        fetch(Text(Release.url_), QDir(Tx).filePath("portable.zip"), Release.zipBytes_, [this, Root, Tx, Release] {
            showProgress(tr("ZIP·파일 해시·코드 서명을 검증하고 있습니다…"));
            run([Root, Tx, Release](std::stop_token Stop) {
                const auto Stage = QDir(Tx).filePath("staging"); CheckPlainPath(Native(Stage)); Require(QDir().mkpath(Stage), "update-staging-directory");
                StageArchive(QDir(Tx).filePath("portable.zip"), Stage, Root, Release, Stop); Require(!Stop.stop_requested(), "update-cancelled");
                VerifyProductSignature(Native(Root) / L"HyperCommanderUpdater.exe"); const auto Helper = Native(Tx) / L"HyperCommanderUpdater.exe";
                Require(CopyFileW((Native(Root) / L"HyperCommanderUpdater.exe").c_str(), Helper.c_str(), TRUE), "update-helper-copy"); VerifyProductSignature(Helper);
                Require(HashFile(Helper) == HashFile(Native(Root) / L"HyperCommanderUpdater.exe"), "update-helper-copy-hash");
                DurableWrite(Native(Root) / L".update/ready.json", "{\"transaction\":" + QuoteJson(Utf8(Native(Tx).filename().wstring())) + "}"); return Result{Release, {}};
            }, [this](Result Value) { release_ = std::move(Value.release_); ready_ = true; if (progress_) progress_->hide(); offer(); });
        });
    });
}
void UpdateController::restore() {
    if (busy_ || ready_) return; const auto Root = root_; const auto Current = currentVersion_;
    if (!QFileInfo::exists(QDir(Root).filePath(".update/ready.json"))) return;
    automatic_ = true; stop_ = std::stop_source{};
    run([Root, Current](std::stop_token Stop) {
        const auto Name = ParseJson(ReadBounded(Native(Root) / L".update/ready.json", 4096)).At("transaction", Json::Kind::String).text_;
        CheckRelative(Name); Require(Name.size() == 36 && QUuid::fromString(Text(Name)).toString(QUuid::WithoutBraces) == Text(Name)
            && !QUuid::fromString(Text(Name)).isNull(), "update-ready-path");
        const auto Tx = Native(Root) / L".update" / Wide(Name); CheckPlainPath(Tx);
        const auto Release = ParseManifest(AuthenticateManifest(Tx / L"update-manifest.cat", Tx / L"update-manifest.json")); Eligible(Release, Current);
        VerifyTree(Tx / L"staging", Release, true, Stop); VerifyProductSignature(Tx / L"HyperCommanderUpdater.exe");
        return Result{Release, {}, QString::fromStdWString(Tx.wstring())};
    }, [this, Root](Result Value) {
        tx_ = std::move(Value.transaction_); release_ = std::move(Value.release_); ready_ = true;
        // The user chooses restart through the same check command after startup.
    });
}
bool UpdateController::LaunchPending() {
    try {
        Require(ready_ && restartRequested_ && canRestart_() && !OtherProductProcesses(Native(root_)), "update-work-still-running");
        const auto Helper = Native(tx_) / L"HyperCommanderUpdater.exe"; VerifyProductSignature(Helper);
        const auto Config = hc::settings::Paths(); Require(Config.error_.isEmpty(), "update-config-directory");
        DurableWrite(Native(tx_) / L"restart.json", "{\"configDirectory\":" + QuoteJson(Config.directory_.toUtf8().toStdString()) + "}");
        FILETIME Created{}, Exit{}, Kernel{}, User{}; Require(GetProcessTimes(GetCurrentProcess(), &Created, &Exit, &Kernel, &User), "update-process-created");
        const auto Stamp = (std::uint64_t(Created.dwHighDateTime) << 32) | Created.dwLowDateTime;
        Require(QProcess::startDetached(QString::fromStdWString(Helper.wstring()), {"--apply", root_, tx_, QString::number(GetCurrentProcessId()), QString::number(Stamp)}, tx_), "update-helper-launch"); return true;
    } catch (const std::exception& Error) { restartRequested_ = false; fail(Error.what()); return false; }
}
}
