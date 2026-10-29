#include "HyperCommanderUpdater/stdafx.h"
#include "HyperCommanderUpdater/updateController.hpp"
#include "HyperCommanderUpdater/updateMessages.hpp"
#include "HyperCommanderUpdater/updateTransaction.hpp"
#include "UniqueLibs/settings/appSettings.hpp"

#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QPixmap>
#include <QLabel>
#include <QLocale>
#include <QProgressBar>
#include <QKeyEvent>
#include <QTemporaryDir>
#include <QProgressDialog>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPushButton>
#include <QStandardPaths>
#include <QTimer>
#include <iostream>

namespace {
class RestartWindow final : public QWidget {
public:
    std::function<bool()> launch_;
protected:
    void closeEvent(QCloseEvent* Event) override {
        if (launch_ && !launch_()) { Event->ignore(); return; }
        QWidget::closeEvent(Event);
    }
};
class FixtureReply final : public QNetworkReply {
public:
    // CutAt drops the connection after that many body bytes; a Range request is answered with 206.
    FixtureReply(const QNetworkRequest& Request, const QString& FileName, bool Redirect, bool Failure, qint64 CutAt, QObject* Parent) : QNetworkReply(Parent), file_(FileName) {
        setRequest(Request); setUrl(Request.url()); setOperation(QNetworkAccessManager::GetOperation); open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        if (Redirect) setAttribute(QNetworkRequest::RedirectionTargetAttribute, QUrl("http://localhost/disallowed"));
        if (!Redirect && !Failure && !file_.open(QIODevice::ReadOnly)) throw std::runtime_error("fixture-open");
        qint64 Offset = 0; const auto Range = Request.rawHeader("Range");
        if (file_.isOpen() && Range.startsWith("bytes=") && Range.endsWith('-')) {
            Offset = Range.mid(6, Range.size() - 7).toLongLong();
            if (!file_.seek(Offset)) throw std::runtime_error("fixture-range");
            setRawHeader("Content-Range", "bytes " + QByteArray::number(Offset) + "-" + QByteArray::number(file_.size() - 1) + "/" + QByteArray::number(file_.size()));
        }
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, Redirect ? 302 : Failure ? 404 : Offset ? 206 : 200);
        if (file_.isOpen()) setHeader(QNetworkRequest::ContentLengthHeader, file_.size() - Offset);
        timer_.setInterval(0); connect(&timer_, &QTimer::timeout, this, [this, Redirect, Failure, CutAt] {
            if (Redirect || Failure) { finish(); return; }
            if (CutAt >= 0 && sent_ >= CutAt) { setError(RemoteHostClosedError, "fixture-cut"); finish(); return; }
            if (!buffer_.isEmpty()) return; buffer_ = file_.read(CutAt >= 0 ? std::min<qint64>(65536, CutAt - sent_) : 65536); sent_ += buffer_.size();
            if (!buffer_.isEmpty()) emit readyRead();
            if (file_.atEnd() && buffer_.isEmpty()) finish();
        }); timer_.start();
    }
    void abort() override { setError(OperationCanceledError, "cancelled"); finish(); }
    qint64 bytesAvailable() const override { return buffer_.size() + QNetworkReply::bytesAvailable(); }
protected:
    qint64 readData(char* Data, qint64 Size) override {
        const auto Count = std::min<qint64>(Size, buffer_.size()); if (!Count) return isFinished() ? -1 : 0;
        memcpy(Data, buffer_.constData(), std::size_t(Count)); buffer_.remove(0, Count); return Count;
    }
private:
    void finish() { if (isFinished()) return; timer_.stop(); setFinished(true); emit finished(); }
    QFile file_; QByteArray buffer_; QTimer timer_; qint64 sent_ = 0;
};
class FixtureNetwork final : public QNetworkAccessManager {
public:
    FixtureNetwork(QString Package, QString Mode, QObject* Parent) : QNetworkAccessManager(Parent), package_(std::move(Package)), mode_(std::move(Mode)) {}
    int requests_ = 0; bool zip_ = false; QList<QByteArray> ranges_; qint64 cut_ = -1;
protected:
    QNetworkReply* createRequest(Operation, const QNetworkRequest& Request, QIODevice*) override {
        ++requests_; auto Name = Request.url().fileName(); zip_ = Name.endsWith(".zip");
        // Resume: the first archive transfer drops halfway; the next must continue from there.
        qint64 CutAt = -1;
        if (zip_) {
            ranges_.append(Request.rawHeader("Range"));
            if (mode_ == "resume" && ranges_.size() == 1) CutAt = cut_ = QFileInfo(QDir(package_).filePath(Name)).size() / 2;
        }
        return new FixtureReply(Request, QDir(package_).filePath(Name), mode_ == "redirect", (mode_ == "http-error" || mode_ == "automatic"), CutAt, this);
    }
private:
    QString package_, mode_;
};
}
namespace hc::update {
class UpdateControllerTest {
public:
    static int CaptureUi(const QString& Directory) {
        if (!QDir().mkpath(Directory)) throw std::runtime_error("capture-directory");
        QWidget Window; Window.resize(800, 480); Window.setWindowTitle("HyperCommander");
        UpdateController Controller(&Window, [] { return false; });
        bool Saved = true;
        Controller.showProgress(QString::fromUtf8("새 버전과 서명을 확인하고 있습니다…")); QCoreApplication::processEvents();
        Saved &= Controller.progress_->grab().save(QDir(Directory).filePath("02-update-checking.png")); Controller.progress_->hide();
        Controller.showProgress(QString::fromUtf8("Portable ZIP을 다운로드하고 있습니다…"));
        auto* Progress = Controller.progress_.data();
        auto* File = Progress->findChild<QLabel*>("hc.update.file");
        auto* Amount = Progress->findChild<QLabel*>("hc.update.amount");
        auto* Percent = Progress->findChild<QLabel*>("hc.update.percent");
        auto* Bar = Progress->findChild<QProgressBar*>("hc.update.bar");
        const QString Name = "HyperCommander-2026.10.29.1-windows-x64-portable.zip";
        const QLocale Locale;
        const std::uint64_t Total = 192382226;
        for (const auto Count : {std::uint64_t(0), Total / 2, Total}) {
            Progress->SetDownloadProgress(Name, Count, Total); QCoreApplication::processEvents();
            if (!File || !Amount || !Percent || !Bar || File->text() != Name || !File->isVisible()
                || Amount->text() != Locale.formattedDataSize(qint64(Count), 1, QLocale::DataSizeIecFormat) + " / " + Locale.formattedDataSize(qint64(Total), 1, QLocale::DataSizeIecFormat)
                || Percent->text() != Locale.toString(double(Count) * 100.0 / double(Total), 'f', 1) + '%'
                || Bar->maximum() != 1000 || Bar->value() != int(Count * 1000 / Total) || Progress->width() > 420)
                throw std::runtime_error("download-progress-content-or-width");
            Saved &= Progress->grab().save(QDir(Directory).filePath("download-" + QString::number(Bar->value()) + ".png"));
        }
        Progress->SetDownloadProgress(Name, Total / 2, 0); QCoreApplication::processEvents();
        if (Bar->maximum() != 0 || Percent->isVisible() || Amount->text().isEmpty()) throw std::runtime_error("unknown-download-total");
        Controller.showProgress(QString::fromUtf8("ZIP·파일 해시·코드 서명을 검증하고 있습니다…")); QCoreApplication::processEvents();
        if (File->isVisible() || Amount->isVisible() || Percent->isVisible() || Bar->maximum() != 0) throw std::runtime_error("stale-download-details");
        Saved &= Progress->grab().save(QDir(Directory).filePath("download-verifying.png"));
        // Exercise the real archive transfer, including an interrupted Range resume.
        QTemporaryDir Transfer(QDir(Directory).filePath("transfer-XXXXXX"));
        if (!Transfer.isValid()) throw std::runtime_error("progress-transfer-directory");
        QFile Archive(Transfer.filePath(Name));
        if (!Archive.open(QIODevice::WriteOnly) || Archive.write(QByteArray(262144, 'x')) != 262144) throw std::runtime_error("progress-transfer-fixture");
        Archive.close();
        delete Controller.network_; auto* Network = new FixtureNetwork(Transfer.path(), "resume", &Controller); Controller.network_ = Network;
        Controller.showProgress(QString::fromUtf8("Portable ZIP을 다운로드하고 있습니다…"));
        Controller.transferStarted_ = QDateTime::currentMSecsSinceEpoch(); Controller.busy_ = true;
        const auto Url = "https://github.com/jgh0721/hyperCommander/releases/download/v2026.10.29.1/" + Name;
        bool Downloaded = false;
        Controller.fetchArchive(Url, Url, Transfer.filePath("portable.part"), 262144, [&] { Downloaded = true; });
        const auto Start = GetTickCount64();
        while (!Downloaded && GetTickCount64() - Start < 10000) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (!Downloaded || Network->ranges_.size() != 2 || !Network->ranges_[0].isEmpty() || Network->ranges_[1] != "bytes=131072-"
            || File->text() != Name || Bar->value() != 1000 || Percent->text() != Locale.toString(100.0, 'f', 1) + '%')
            throw std::runtime_error("download-resume-progress");
        Controller.busy_ = false;
        QKeyEvent Escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier); QCoreApplication::sendEvent(Progress, &Escape);
        if (!Progress->wasCanceled() || Progress->isVisible() || !Controller.stop_.stop_requested()) throw std::runtime_error("progress-escape-cancel");
        Controller.stop_ = std::stop_source{}; Controller.showProgress("cancel");
        Progress->findChild<QPushButton*>()->click();
        if (!Progress->wasCanceled() || !Controller.stop_.stop_requested()) throw std::runtime_error("progress-button-cancel");
        Controller.stop_ = std::stop_source{}; Controller.showProgress("close"); Progress->close();
        if (!Progress->wasCanceled() || !Controller.stop_.stop_requested()) throw std::runtime_error("progress-close-cancel");
        Controller.release_.version_ = "2026.10.18.4"; Controller.release_.zipBytes_ = 192382226;
        Controller.release_.notes_ = "https://github.com/jgh0721/hyperCommander/releases/tag/v2026.10.18.4";
        for (const bool Ready : {false, true}) {
            Controller.ready_ = Ready;
            QTimer::singleShot(250, &Window, [&, Ready] {
                for (auto* Widget : QApplication::topLevelWidgets()) {
                    auto* Message = qobject_cast<QMessageBox*>(Widget);
                    if (!Message || !Message->isVisible()) continue;
                    Saved &= Message->grab().save(QDir(Directory).filePath(Ready ? "04-update-ready.png" : "03-update-available.png"));
                    for (auto* Button : Message->buttons()) if (Button->text() == QString::fromUtf8("나중에")) { Button->click(); break; }
                }
            });
            Controller.offer();
        }
        if (!Saved) throw std::runtime_error("capture-save");
        std::cout << "Progress content, compact width, resumed transfer and cancellation: PASS. Production dialogs captured; no external network or installation performed.\n";
        return 0;
    }
    static int Run(const QString& Root, const QString& Package, const QString& Mode) {
        RestartWindow Window;
        UpdateController Controller(&Window, [&] { return Mode == "restart" || Mode == "launch-failure"; });
        if (Mode == "launch-failure") {
            Window.launch_ = [&] { return Controller.LaunchPending(); };
            Window.show();
        }
        Controller.root_ = Root; Controller.currentVersion_ = ParseInventory(ReadBounded(Path(Root.toStdWString()) / L"product-inventory.json", 1048576)).version_;
        delete Controller.network_; auto* Network = new FixtureNetwork(Package, Mode, &Controller); Controller.network_ = Network;
        // Reproduce a normal background plugin host using an isolated copy of
        // the probe. Restore the fixture's signed product bytes before returning.
        QProcess Host; QByteArray OriginalHost; const auto HostImage = QDir(Root).filePath("hc-plugin-host-x64.exe");
        const auto RestoreHost = qScopeGuard([&] {
            if (Host.state() != QProcess::NotRunning) { Host.kill(); Host.waitForFinished(5000); }
            if (!OriginalHost.isEmpty()) { QFile::remove(HostImage); QFile File(HostImage); if (!File.open(QIODevice::WriteOnly) || File.write(OriginalHost) != OriginalHost.size()) qFatal("fixture-host-restore"); }
        });
        if (Mode == "restart") {
            QFile Original(HostImage); if (!Original.open(QIODevice::ReadOnly)) throw std::runtime_error("fixture-host-original");
            OriginalHost = Original.readAll(); Original.close();
            if (!QFile::remove(HostImage) || !QFile::copy(QCoreApplication::applicationDirPath() + "/hc_update_tests.exe", HostImage)) throw std::runtime_error("fixture-host-copy");
            auto Environment = QProcessEnvironment::systemEnvironment();
            Environment.insert("PATH", QCoreApplication::applicationDirPath() + ";" + Environment.value("PATH")); Host.setProcessEnvironment(Environment);
            Host.start(HostImage, {"process-fixture", "hidden", "0"});
            if (!Host.waitForStarted(5000) || !Host.waitForReadyRead(5000) || !Host.readAllStandardOutput().contains("ready")) throw std::runtime_error("fixture-host-start");
            const Path NativeRoot(Root.toStdWString());
            if (!OtherProductProcesses(NativeRoot) || OtherProductProcesses(NativeRoot, 0, ProductProcessCheck::BeforeRestart)) throw std::runtime_error("fixture-host-policy");
        }
        if (Mode == "automatic" || Mode == "minimum-version") {
            auto General = hc::settings::AppSettings().Read(hc::settings::Section::General).toObject();
            General["update"] = QJsonObject{{"checkIntervalDays", Mode == "automatic" ? 1 : 0}, {"automaticCheck", Mode == "automatic"},
                {"lastAttemptUtc", QDateTime::currentDateTimeUtc().addSecs(-86401).toString(Qt::ISODate)}};
            if (!hc::settings::AppSettings().Put(hc::settings::Section::General, General)) throw std::runtime_error("schedule-settings");
        }
        int Heartbeats = 0, Offers = 0, Errors = 0; bool Finished = false, Blocked = false; QTimer Timer;
        QObject::connect(&Timer, &QTimer::timeout, &Window, [&] {
            ++Heartbeats;
            if (Mode == "cancel" && Network->zip_) Controller.cancel();
            for (auto* Widget : QApplication::topLevelWidgets()) if (auto* Message = qobject_cast<QMessageBox*>(Widget); Message && Message->isVisible()) {
                if (Message->text().contains("완료하지 못했습니다")) {
                    ++Errors;
                    const auto Failure = ParseJson(ReadBounded(Path(Root.toStdWString()) / L".update/last-error.json", 32768)).At("error", Json::Kind::String).text_;
                    if (Failure.empty() || !Message->text().contains(QString::fromUtf8(Failure))) throw std::runtime_error("service-error-not-persisted");
                    if (Mode == "minimum-version" && Failure != "이 버전에서는 Portable ZIP을 직접 설치해야 합니다.")
                        throw std::runtime_error("incompatible-update-manual-install-message");
                }
                else ++Offers;
                if (Message->text().contains("파일 작업·명령 실행")) Blocked = true;
                for (auto* Button : Message->buttons()) {
                    if (Mode == "launch-failure" && Controller.ready_ && Offers == 2 && !Errors) {
                        const Path Tx(Controller.tx_.toStdWString());
                        // Restore a previously packaged helper, then invalidate
                        // staging after preparation. Launch must refresh the
                        // helper and report preflight failure without closing.
                        if (!CopyFileW((Tx / L"staging/HyperCommanderUpdater.exe").c_str(), (Tx / L"HyperCommanderUpdater.exe").c_str(), FALSE))
                            throw std::runtime_error("fixture-stale-helper-copy");
                        DurableWrite(Tx / L"staging/product-inventory.json", "invalid-stage");
                    }
                    const bool Accept = Offers == 1 && !Controller.ready_ || (Mode == "blocked" || Mode == "restart" || Mode == "launch-failure") && Offers == 2 || Blocked;
                    if ((Accept && Message->buttonRole(Button) == QMessageBox::AcceptRole)
                        || ((!Accept || Errors) && Message->buttonRole(Button) == QMessageBox::RejectRole)
                        || Message->buttonRole(Button) == QMessageBox::AcceptRole && Errors) { Button->click(); break; }
                }
            }
            if (!Controller.busy_ && !Controller.reply_ && (Controller.ready_ || Errors || Mode == "cancel" && Controller.stop_.stop_requested() || Mode == "automatic" && Network->requests_ > 0)) Finished = true;
        }); Timer.start(5); if (Mode == "restore") Controller.restore(); else if (Mode == "automatic") Controller.checkAutomatically(); else Controller.Check();
        const auto Start = GetTickCount64(); while (!Finished && GetTickCount64() - Start < 120000) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        const bool Prepared = Mode == "success" || Mode == "blocked" || Mode == "restore" || Mode == "restart" || Mode == "launch-failure" || Mode == "resume";
        if (!Finished || (Prepared ? !Controller.ready_ || (Mode != "restore" && (Offers < 2 || Network->requests_ != (Mode == "resume" ? 4 : 3) || Heartbeats < 5)) : Controller.ready_)) throw std::runtime_error("service-state-or-liveness");
        if (Mode == "resume" && (Errors || Network->ranges_.size() != 2 || !Network->ranges_[0].isEmpty() || Network->ranges_[1] != "bytes=" + QByteArray::number(Network->cut_) + "-"))
            throw std::runtime_error("interrupted-download-not-resumed");
        if (Mode == "restore" && (Network->requests_ != 0 || Offers != 1)
            || Mode == "blocked" && (!Blocked || Controller.restartRequested_)) throw std::runtime_error("service-restore-or-blocked-restart");
        if (Mode == "restart" && !Controller.restartRequested_) throw std::runtime_error("own-background-host-blocked-restart");
        if (Mode == "launch-failure" && (Errors != 1 || Controller.restartRequested_ || !Window.isVisible()
            || !QFileInfo::exists(QDir(Root).filePath(".update/ready.json"))
            || QFileInfo::exists(QDir(Root).filePath(".update/active.json"))
            || HashFile(Path(Root.toStdWString()) / L"HyperCommanderUpdater.exe") != HashFile(Path(Controller.tx_.toStdWString()) / L"HyperCommanderUpdater.exe")))
            throw std::runtime_error("failed-helper-closed-parent-or-stale-helper");
        if (Mode == "automatic") {
            if (Network->requests_ != 1 || Errors || Offers || Network->zip_ || !Controller.automatic_)
                throw std::runtime_error("automatic-check-not-background");
            Controller.checkAutomatically();
            if (Controller.busy_ || Network->requests_ != 1) throw std::runtime_error("automatic-check-repeated-too-soon");
            const auto Failure = ParseJson(ReadBounded(Path(Root.toStdWString()) / L".update/last-error.json", 32768)).At("error", Json::Kind::String).text_;
            if (Failure != "update-download-incomplete") throw std::runtime_error("automatic-check-error-not-persisted");
        }
        if (Mode == "redirect" && Network->requests_ != 1) throw std::runtime_error("redirect-was-followed");
        if (Mode == "minimum-version" && (Errors != 1 || Offers != 0 || Network->requests_ != 2 || Network->zip_
            || Controller.ready_ || Controller.restartRequested_
            || QFileInfo::exists(QDir(Root).filePath(".update/ready.json"))
            || QFileInfo::exists(QDir(Root).filePath(".update/active.json"))))
            throw std::runtime_error("incompatible-update-not-blocked-before-download");
        std::cout << "Service " << Mode.toStdString() << ": PASS, requests=" << Network->requests_ << ", heartbeats=" << Heartbeats << '\n'; return 0;
    }
    // Offline cases for the health check retry and user guidance. Root is an installed fixture.
    static int Health(const QString& Root) {
        const Path NativeRoot(Root.toStdWString()); unsigned Passed = 0;
        RestartWindow Window; UpdateController Controller(&Window, [] { return true; });
        Controller.root_ = Root; Controller.currentVersion_ = ParseInventory(ReadBounded(NativeRoot / L"product-inventory.json", 1048576)).version_;
        delete Controller.network_; auto* Network = new FixtureNetwork({}, "http-error", &Controller); Controller.network_ = Network;
        // An active update whose journal cannot be read: the check retries the health
        // check, keeps the update active and reports why instead of a generic code.
        const auto Tx = NativeRoot / L".update/00000000-0000-4000-8000-0000000000aa";
        std::filesystem::create_directories(Tx); DurableWrite(Tx / L"journal.json", "{}");
        DurableWrite(NativeRoot / L".update/active.json", "{\"transaction\":\"00000000-0000-4000-8000-0000000000aa\"}");
        const auto Cleanup = qScopeGuard([&] { DeleteFileW((NativeRoot / L".update/active.json").c_str()); std::error_code Ignored; std::filesystem::remove_all(Tx, Ignored); });
        QString Shown; QTimer Timer; QObject::connect(&Timer, &QTimer::timeout, &Window, [&] {
            for (auto* Widget : QApplication::topLevelWidgets()) if (auto* Message = qobject_cast<QMessageBox*>(Widget); Message && Message->isVisible()) { Shown = Message->text(); Message->accept(); }
        }); Timer.start(5);
        Controller.Check();
        const auto Start = GetTickCount64(); while ((Controller.busy_ || Shown.isEmpty()) && GetTickCount64() - Start < 60000) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        const auto Failure = ParseJson(ReadBounded(NativeRoot / L".update/last-error.json", 32768)).At("error", Json::Kind::String).text_;
        if (!std::string_view(Failure).starts_with("update-health-check-failed: update-json-field") || Network->requests_ != 0
            || !Shown.contains(QString::fromUtf8(Failure)) || !Shown.contains("--recover") || !std::filesystem::exists(NativeRoot / L".update/active.json"))
            throw std::runtime_error("health-check-retry: " + Failure + " / " + Shown.toStdString());
        ++Passed;
        // Guidance keeps the code and adds what the user can do.
        if (Explain("update-disk-space: 1700 MiB", NativeRoot).find(L"1700 MiB") == std::wstring::npos
            || Explain("update-elevated-process", NativeRoot).empty() || Explain("update-root-not-writable: C:\\x", NativeRoot).empty()
            || !Explain("update-cancelled", NativeRoot).empty()) throw std::runtime_error("update-guidance");
        ++Passed;
        std::cout << "Service health: " << Passed << " passed\n"; return 0;
    }
};
}
int main(int Count, char** Args) {
    QStandardPaths::setTestModeEnabled(true); QApplication App(Count, Args); const auto Arguments = App.arguments();
    try {
        if (Arguments.size() == 5 && Arguments[1] == "--capture-ui" && Arguments[3] == "--config-dir")
            return hc::update::UpdateControllerTest::CaptureUi(Arguments[2]);
        if (Arguments.size() != 6 || Arguments[4] != "--config-dir") throw std::runtime_error("service-test-arguments");
        if (Arguments[3] == "health") return hc::update::UpdateControllerTest::Health(Arguments[1]);
        return hc::update::UpdateControllerTest::Run(Arguments[1], Arguments[2], Arguments[3]);
    } catch (const std::exception& Error) { std::cerr << Error.what() << '\n'; return 1; }
}
