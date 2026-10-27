#pragma once
#include "updateProtocol.hpp"
#include <QDialog>
#include <QObject>
#include <QPointer>
#include <QString>
#include <functional>
#include <stop_token>

class QWidget;
class QNetworkAccessManager;
class QNetworkReply;
class QLabel;
class QProgressBar;
namespace hc::update {
class UpdateControllerTest;
/// Update progress in the product dialog frame: header band, message and bar, footer band with Cancel.
/// It keeps the QProgressDialog names the controller calls; Cancel, Esc and the close box all cancel.
class UpdateProgressDialog final : public QDialog {
public:
    UpdateProgressDialog(const QString& Label, QWidget* Parent);
    void setLabelText(const QString& Text);
    void setRange(int Minimum, int Maximum);
    void setValue(int Value);
    bool wasCanceled() const { return canceled_; }
    void reject() override;
    std::function<void()> Canceled;
protected:
    void showEvent(QShowEvent* Event) override;
private:
    QLabel* label_ = nullptr;
    QProgressBar* bar_ = nullptr;
    bool canceled_ = false;
};
class UpdateController final : public QObject {
public:
    UpdateController(QWidget* Window, std::function<bool()> CanRestart);
    ~UpdateController() override;
    void Check();
    bool RestartRequested() const { return restartRequested_; }
    bool LaunchPending();
private:
    friend class UpdateControllerTest;
    struct Result { Manifest release_; QString error_, transaction_; };
    void check(bool Automatic);
    void checkAutomatically();
    void offer();
    void download();
    void restore();
    void cancel();
    void fail(const QString& Error);
    void showProgress(const QString& Text);
    void fetch(const QString& Url, const QString& Destination, std::uint64_t Limit, std::function<void()> Done, int Redirects = 0);
    void fetchArchive(const QString& Origin, const QString& Url, const QString& Partial, std::uint64_t Limit, std::function<void()> Done, int Redirects = 0, int Attempt = 0);
    void run(std::function<Result(std::stop_token)> Work, std::function<void(Result)> Done);
    QWidget* window_;
    std::function<bool()> canRestart_;
    QNetworkAccessManager* network_;
    QPointer<QNetworkReply> reply_;
    QPointer<UpdateProgressDialog> progress_;
    QString root_, tx_;
    std::string currentVersion_;
    Manifest release_;
    std::stop_source stop_;
    bool busy_ = false, automatic_ = false, ready_ = false, restartRequested_ = false;
    qint64 transferStarted_ = 0;
};
}
