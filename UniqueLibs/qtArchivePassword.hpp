#pragma once
#include "commonLibs/storage/storage.hpp"
#include <QApplication>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <future>
namespace hc::plugins
{
inline hc::storage::Result<hc::storage::Password> PromptArchivePassword(const std::string &Archive, unsigned Attempt,
                                                                        const hc::storage::OperationContext &Context)
{
    using namespace hc::storage;
    if (!qApp || QCoreApplication::closingDown())
        return Error{ErrorCategory::Cancelled, "archive-password-cancelled", {"wcx"}};
    auto Promise = std::make_shared<std::promise<Result<Password>>>();
    auto Future = Promise->get_future();
    auto Abandoned = std::make_shared<std::atomic_bool>(false);
    const auto Show = [Promise, Abandoned, Archive, Attempt, Context]
    {
        if (Abandoned->load() || Context.Check({"wcx"}))
        {
            Promise->set_value(Error{ErrorCategory::Cancelled, "archive-password-cancelled", {"wcx"}});
            return;
        }
        QInputDialog Dialog(QApplication::activeWindow());
        Dialog.setObjectName("hc.archive.password");
        Dialog.setWindowTitle(QObject::tr("압축 파일 암호"));
        Dialog.setTextEchoMode(QLineEdit::Password);
        Dialog.setLabelText(
            (Attempt > 1 ? QObject::tr("암호를 다시 입력하세요.\n") : QObject::tr("압축 파일의 암호를 입력하세요.\n")) +
            QString::fromStdString(Archive));
        if (auto *Label = Dialog.findChild<QLabel *>())
            Label->setTextFormat(Qt::PlainText);
        if (auto *Edit = Dialog.findChild<QLineEdit *>())
            Edit->setMaxLength(4096);
        QTimer Timer;
        QObject::connect(&Timer, &QTimer::timeout, &Dialog,
                         [&]
                         {
                             if (Abandoned->load() || Context.Check({"wcx"}))
                                 Dialog.reject();
                         });
        Timer.start(50);
        const bool Accepted = Dialog.exec() == QDialog::Accepted;
        auto Text = Dialog.textValue();
        Dialog.setTextValue({});
        if (Accepted && !Abandoned->load() && !Context.Check({"wcx"}))
            Promise->set_value(Password(Text.toStdString()));
        else
            Promise->set_value(Error{ErrorCategory::Cancelled, "archive-password-cancelled", {"wcx"}});
        Text.fill(QChar::Null);
    };
    if (QThread::currentThread() == qApp->thread())
        Show();
    else if (!QMetaObject::invokeMethod(qApp, Show, Qt::QueuedConnection))
        return Error{ErrorCategory::Cancelled, "archive-password-cancelled", {"wcx"}};
    while (Future.wait_for(std::chrono::milliseconds(25)) != std::future_status::ready)
        if (const auto Stop = Context.Check({"wcx"}))
        {
            Abandoned->store(true);
            return *Stop;
        }
    return Future.get();
}
} // namespace hc::plugins
