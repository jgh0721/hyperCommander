#pragma once
#include <QWidget>
#include <QJsonObject>
#include <QLocalServer>
#include <QPointer>
#include <QTimer>
class QLocalSocket;
class QProcess;
namespace hc::document
{
class MarkdownProcessView final : public QWidget
{
    Q_OBJECT
  public:
    explicit MarkdownProcessView(QWidget *Parent = nullptr);
    ~MarkdownProcessView() override;
    void Send(const QJsonObject &Message);
    void Restart();
    void Stop();
    bool HasFocus() const;
    void FocusInput();
  signals:
    void Reported(const QJsonObject &Message);

  protected:
    void resizeEvent(QResizeEvent *) override;
    void focusInEvent(QFocusEvent *) override;

  private:
    void start();
    void receive();
    void resizeChild();
    QLocalServer server_;
    QPointer<QLocalSocket> socket_;
    QPointer<QProcess> process_;
    QByteArray input_;
    QString token_;
    // Replayed when the helper (re)attaches.
    QJsonObject pending_, shortcuts_, zoom_;
    QTimer startup_;
    quintptr child_ = 0;
    void *job_ = nullptr;
    bool ready_ = false, stopping_ = false, focusRequested_ = false;
};
} // namespace hc::document
