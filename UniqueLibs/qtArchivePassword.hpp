#pragma once
#include "commonLibs/storage/storage.hpp"
#include "UniqueLibs/passwords/passwordUi.hpp"
#include <QApplication>
#include <QThread>
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
        auto Reply = hc::passwords::AcquireArchivePassword(Archive, Attempt, Context, QApplication::activeWindow());
        if (Abandoned->load() || Context.Check({"wcx"}))
            Promise->set_value(Error{ErrorCategory::Cancelled, "archive-password-cancelled", {"wcx"}});
        else Promise->set_value(std::move(Reply));
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
