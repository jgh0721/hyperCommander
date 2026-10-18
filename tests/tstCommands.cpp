#include "commonLibs/commands/commandBus.hpp"
#include <QtTest>
#include <thread>

using namespace hc::commands;
using namespace hc::storage;

class QueueExecutor final : public IExecutor
{
public:
    bool Post(std::function<void()> Work) override
    {
        std::lock_guard Lock(mutex_);
        if (closed_) return false;
        queue_.push_back(std::move(Work));
        return true;
    }
    void Drain()
    {
        for (;;)
        {
            std::function<void()> Work;
            { std::lock_guard Lock(mutex_); if (queue_.empty()) break; Work = std::move(queue_.front()); queue_.pop_front(); }
            Work();
        }
    }
    void Close() { std::lock_guard Lock(mutex_); closed_ = true; queue_.clear(); }
private:
    std::mutex mutex_;
    std::deque<std::function<void()>> queue_;
    bool closed_ = false;
};

class CommandTests : public QObject
{
    Q_OBJECT
private slots:
    void schemaPermissionAndExecutor()
    {
        auto Registry = std::make_shared<CommandRegistry>();
        auto Hub = std::make_shared<EventHub>();
        auto Executor = std::make_shared<QueueExecutor>();
        std::vector<EventEnvelope> Events;
        auto Subscription = Hub->Subscribe("", Executor, [&](const auto& Event) { Events.push_back(Event); });
        std::thread::id HandlerThread;
        int Calls = 0;
        CommandDescriptor Descriptor{"hc.navigate.path", 1, {{"path", {ParameterType::String}}}};
        QVERIFY(Registry->Register(Descriptor, {{}, [&](const CommandInvocation&) -> Result<CommandOutcome> {
            HandlerThread = std::this_thread::get_id(); ++Calls; return CommandOutcome{};
        }}, Executor).HasValue());
        CommandDispatcher Dispatcher(Registry, Hub);
        CommandInvocation Invocation;
        Invocation.id_ = Descriptor.id_;
        Invocation.parameters_ = {{"path", std::string("C:\\한글")}};
        QCOMPARE(Dispatcher.Dispatch(Invocation).get().Failure().category_, ErrorCategory::AccessDenied);
        Invocation.permissions_.insert("navigation");
        Invocation.schemaVersion_ = 2;
        QCOMPARE(Dispatcher.Dispatch(Invocation).get().Failure().code_, std::string("schema-version"));
        Invocation.schemaVersion_ = 1;
        Invocation.parameters_["path"] = true;
        QVERIFY(!Dispatcher.Dispatch(Invocation).get().HasValue());
        Invocation.parameters_["path"] = std::string("C:\\한글");
        Invocation.correlationId_ = "corr-1";
        Invocation.source_ = "shortcut";
        auto Result = Dispatcher.Dispatch(Invocation);
        QCOMPARE(Result.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);
        std::thread Worker([&] { Executor->Drain(); });
        const auto WorkerId = Worker.get_id();
        Worker.join();
        QVERIFY(Result.get().HasValue());
        QCOMPARE(Calls, 1);
        QVERIFY(HandlerThread == WorkerId);
        QCOMPARE(Events.size(), 2u);
        QCOMPARE(Events[0].id_, std::string("command.started"));
        QCOMPARE(Events[1].id_, std::string("command.completed"));
        QCOMPARE(Events[1].correlationId_, std::string("corr-1"));
        QVERIFY(Events[0].sequence_ < Events[1].sequence_);
        Registry->Unregister(Descriptor.id_);
        QCOMPARE(Dispatcher.Dispatch(Invocation).get().Failure().category_, ErrorCategory::NotFound);
    }
    void boundedCoalescingAndLifetime()
    {
        EventHub Hub;
        auto Executor = std::make_shared<QueueExecutor>();
        std::vector<EventEnvelope> Events;
        auto Subscription = Hub.Subscribe("", Executor, [&](const auto& Event) { Events.push_back(Event); }, 2);
        EventEnvelope Progress;
        Progress.id_ = "job.progress";
        Progress.correlationId_ = "job-1";
        Progress.progress_ = true;
        Hub.Publish(Progress);
        EventEnvelope Complete;
        Complete.id_ = "job.completed";
        Hub.Publish(Complete);
        QCOMPARE(Hub.Publish(Progress).coalesced_, 1u);
        QCOMPARE(Hub.Publish(Complete).rejected_, 1u);
        Executor->Drain();
        QCOMPARE(Events.size(), 2u);
        QVERIFY(Events[0].sequence_ < Events[1].sequence_);
        Hub.Publish(Complete);
        Subscription.Value().Reset();
        Executor->Drain();
        QCOMPARE(Events.size(), 2u);
    }
    void concurrentOrderingAndReentrancy()
    {
        EventHub Hub;
        auto Executor = std::make_shared<QueueExecutor>();
        std::vector<std::uint64_t> Sequences;
        auto Subscription = Hub.Subscribe("work", Executor, [&](const auto& Event) { Sequences.push_back(Event.sequence_); }, 256);
        auto Publish = [&] { for (int Index = 0; Index < 100; ++Index) { EventEnvelope Event; Event.id_ = "work"; Hub.Publish(Event); } };
        std::thread A(Publish), B(Publish);
        A.join(); B.join();
        Executor->Drain();
        QCOMPARE(Sequences.size(), 200u);
        QVERIFY(std::is_sorted(Sequences.begin(), Sequences.end()));
        int Reentrant = 0;
        auto Other = Hub.Subscribe("nested", Executor, [&](const auto& Event) { if (++Reentrant < 3) Hub.Publish(Event); });
        EventEnvelope Event; Event.id_ = "nested";
        Hub.Publish(Event); Executor->Drain();
        QCOMPARE(Reentrant, 3);
    }
    void closedExecutorCancellationAndErrors()
    {
        auto Registry = std::make_shared<CommandRegistry>();
        auto Hub = std::make_shared<EventHub>();
        auto Executor = std::make_shared<QueueExecutor>();
        QVERIFY(Registry->Register({"test"}, {{}, [](const auto&) -> Result<CommandOutcome> { throw std::runtime_error("failure"); }}, Executor).HasValue());
        CommandDispatcher Dispatcher(Registry, Hub);
        CommandInvocation Invocation;
        Invocation.id_ = "test";
        Invocation.permissions_.insert("navigation");
        auto Failed = Dispatcher.Dispatch(Invocation); Executor->Drain();
        QCOMPARE(Failed.get().Failure().category_, ErrorCategory::Internal);
        std::stop_source Stop; Stop.request_stop();
        Invocation.operation_.stopToken_ = Stop.get_token();
        auto Cancelled = Dispatcher.Dispatch(Invocation); Executor->Drain();
        QCOMPARE(Cancelled.get().Failure().category_, ErrorCategory::Cancelled);
        Invocation.operation_ = {};
        auto Pending = Dispatcher.Dispatch(Invocation);
        Executor->Close();
        QCOMPARE(Pending.get().Failure().code_, std::string("executor-closed"));
        QCOMPARE(Dispatcher.Dispatch(Invocation).get().Failure().category_, ErrorCategory::Cancelled);
    }
};
QTEST_GUILESS_MAIN(CommandTests)
#include "tstCommands.moc"
