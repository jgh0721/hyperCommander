#pragma once
#include "updateProtocol.hpp"
#include <QObject>
#include <QPointer>
#include <QString>
#include <functional>
#include <stop_token>

class QMenu;
class QWidget;
class QNetworkAccessManager;
class QNetworkReply;
class QProgressDialog;
namespace hc::update {
class UpdateControllerTest;
class UpdateController final : public QObject {
public:
    UpdateController(QWidget* Window, QMenu* Menu, std::function<bool()> CanRestart);
    ~UpdateController() override;
    void Check();
    bool RestartRequested() const { return restartRequested_; }
    bool LaunchPending();
private:
    friend class UpdateControllerTest;
    struct Result { Manifest release_; QString error_, transaction_; };
    void check(bool Automatic);
    void offer();
    void download();
    void restore();
    void cancel();
    void fail(const QString& Error);
    void showProgress(const QString& Text);
    void fetch(const QString& Url, const QString& Destination, std::uint64_t Limit, std::function<void()> Done, int Redirects = 0);
    void run(std::function<Result(std::stop_token)> Work, std::function<void(Result)> Done);
    QWidget* window_;
    std::function<bool()> canRestart_;
    QNetworkAccessManager* network_;
    QPointer<QNetworkReply> reply_;
    QPointer<QProgressDialog> progress_;
    QString root_, tx_;
    std::string currentVersion_;
    Manifest release_;
    std::stop_source stop_;
    bool busy_ = false, automatic_ = false, ready_ = false, restartRequested_ = false;
    qint64 transferStarted_ = 0;
};
}
