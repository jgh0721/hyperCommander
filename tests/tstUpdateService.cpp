#include "HyperCommanderUpdater/stdafx.h"
#include "HyperCommanderUpdater/updateController.hpp"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QMenu>
#include <QMessageBox>
#include <QPixmap>
#include <QProgressDialog>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPushButton>
#include <QStandardPaths>
#include <QTimer>
#include <iostream>

namespace {
class FixtureReply final : public QNetworkReply {
public:
    FixtureReply(const QNetworkRequest& Request, const QString& FileName, bool Redirect, bool Failure, QObject* Parent) : QNetworkReply(Parent), file_(FileName) {
        setRequest(Request); setUrl(Request.url()); setOperation(QNetworkAccessManager::GetOperation); open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, Redirect ? 302 : Failure ? 404 : 200);
        if (Redirect) setAttribute(QNetworkRequest::RedirectionTargetAttribute, QUrl("http://localhost/disallowed"));
        if (!Redirect && !Failure && !file_.open(QIODevice::ReadOnly)) throw std::runtime_error("fixture-open");
        if (file_.isOpen()) setHeader(QNetworkRequest::ContentLengthHeader, file_.size());
        timer_.setInterval(0); connect(&timer_, &QTimer::timeout, this, [this, Redirect, Failure] {
            if (Redirect || Failure) { finish(); return; }
            if (!buffer_.isEmpty()) return; buffer_ = file_.read(65536); if (!buffer_.isEmpty()) emit readyRead();
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
    QFile file_; QByteArray buffer_; QTimer timer_;
};
class FixtureNetwork final : public QNetworkAccessManager {
public:
    FixtureNetwork(QString Package, QString Mode, QObject* Parent) : QNetworkAccessManager(Parent), package_(std::move(Package)), mode_(std::move(Mode)) {}
    int requests_ = 0; bool zip_ = false;
protected:
    QNetworkReply* createRequest(Operation, const QNetworkRequest& Request, QIODevice*) override {
        ++requests_; auto Name = Request.url().fileName(); zip_ = Name.endsWith(".zip");
        return new FixtureReply(Request, QDir(package_).filePath(Name), mode_ == "redirect", mode_ == "http-error", this);
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
        QMenu Menu(QString::fromUtf8("도움말"), &Window);
        UpdateController Controller(&Window, &Menu, [] { return false; });
        bool Saved = true;
        Menu.popup(QPoint(300, 200)); QCoreApplication::processEvents();
        Saved &= Menu.grab().save(QDir(Directory).filePath("01-update-menu.png")); Menu.hide();
        Controller.showProgress(QString::fromUtf8("새 버전과 서명을 확인하고 있습니다…")); QCoreApplication::processEvents();
        Saved &= Controller.progress_->grab().save(QDir(Directory).filePath("02-update-checking.png")); Controller.progress_->hide();
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
        std::cout << "Production update menu and dialogs captured with a simulated release; no network or installation performed.\n";
        return 0;
    }
    static int Run(const QString& Root, const QString& Package, const QString& Mode) {
        QWidget Window; QMenu Menu; UpdateController Controller(&Window, &Menu, [] { return false; });
        Controller.root_ = Root; Controller.currentVersion_ = ParseInventory(ReadBounded(Path(Root.toStdWString()) / L"product-inventory.json", 1048576)).version_;
        delete Controller.network_; auto* Network = new FixtureNetwork(Package, Mode, &Controller); Controller.network_ = Network;
        int Heartbeats = 0, Offers = 0, Errors = 0; bool Finished = false, Blocked = false; QTimer Timer;
        QObject::connect(&Timer, &QTimer::timeout, &Window, [&] {
            ++Heartbeats;
            if (Mode == "cancel" && Network->zip_) Controller.cancel();
            for (auto* Widget : QApplication::topLevelWidgets()) if (auto* Message = qobject_cast<QMessageBox*>(Widget); Message && Message->isVisible()) {
                if (Message->text().contains("완료하지 못했습니다")) {
                    ++Errors;
                    const auto Failure = ParseJson(ReadBounded(Path(Root.toStdWString()) / L".update/last-error.json", 32768)).At("error", Json::Kind::String).text_;
                    if (Failure.empty() || !Message->text().contains(QString::fromUtf8(Failure))) throw std::runtime_error("service-error-not-persisted");
                }
                else ++Offers;
                if (Message->text().contains("파일 작업·명령 실행")) Blocked = true;
                for (auto* Button : Message->buttons()) {
                    const bool Accept = Offers == 1 && !Controller.ready_ || Mode == "blocked" && Offers == 2 || Blocked;
                    if ((Accept && Message->buttonRole(Button) == QMessageBox::AcceptRole)
                        || ((!Accept || Errors) && Message->buttonRole(Button) == QMessageBox::RejectRole)
                        || Message->buttonRole(Button) == QMessageBox::AcceptRole && Errors) { Button->click(); break; }
                }
            }
            if (!Controller.busy_ && !Controller.reply_ && (Controller.ready_ || Errors || Mode == "cancel" && Controller.stop_.stop_requested())) Finished = true;
        }); Timer.start(5); if (Mode == "restore") Controller.restore(); else Controller.Check();
        const auto Start = GetTickCount64(); while (!Finished && GetTickCount64() - Start < 120000) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        const bool Prepared = Mode == "success" || Mode == "blocked" || Mode == "restore";
        if (!Finished || (Prepared ? !Controller.ready_ || (Mode != "restore" && (Offers < 2 || Network->requests_ != 3 || Heartbeats < 5)) : Controller.ready_)) throw std::runtime_error("service-state-or-liveness");
        if (Mode == "restore" && Network->requests_ != 0 || Mode == "blocked" && (!Blocked || Controller.restartRequested_)) throw std::runtime_error("service-restore-or-blocked-restart");
        if (Mode == "redirect" && Network->requests_ != 1) throw std::runtime_error("redirect-was-followed");
        std::cout << "Service " << Mode.toStdString() << ": PASS, requests=" << Network->requests_ << ", heartbeats=" << Heartbeats << '\n'; return 0;
    }
};
}
int main(int Count, char** Args) {
    QStandardPaths::setTestModeEnabled(true); QApplication App(Count, Args); const auto Arguments = App.arguments();
    try {
        if (Arguments.size() == 5 && Arguments[1] == "--capture-ui" && Arguments[3] == "--config-dir")
            return hc::update::UpdateControllerTest::CaptureUi(Arguments[2]);
        if (Arguments.size() != 6 || Arguments[4] != "--config-dir") throw std::runtime_error("service-test-arguments");
        return hc::update::UpdateControllerTest::Run(Arguments[1], Arguments[2], Arguments[3]);
    } catch (const std::exception& Error) { std::cerr << Error.what() << '\n'; return 1; }
}
