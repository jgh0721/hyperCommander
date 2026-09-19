#include "UniqueLibs/qtAutomation.hpp"
#include <QtTest>
#include <QDialog>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QLabel>
#include <QCheckBox>

using namespace hc::automation;
class AutomationTests : public QObject {
    Q_OBJECT
private slots:
    void parserDiagnosticsAndLimits() {
        Diagnostic Details;
        auto Parsed = Compile("# comment\n await invoke \"hc.test\" {\"name\":\"한글\\nvalue\",\"count\":9223372036854775807,\"enabled\":false}\n", &Details);
        QVERIFY(Parsed.HasValue()); QCOMPARE(Parsed.Value().size(), 1u); QCOMPARE(Parsed.Value()[0].line_, 2u);
        QCOMPARE(std::get<std::int64_t>(Parsed.Value()[0].parameters_.at("count")), INT64_MAX);
        QCOMPARE(std::get<std::string>(Parsed.Value()[0].parameters_.at("name")), std::string("한글\nvalue"));
        for (const auto& Bad : {"", "invokeX \"a\" {}", "invoke \"a\" {\"x\":1,\"x\":2}", "invoke \"a\" {\"x\":1,\"\\u0078\":2}",
            "invoke \"a\" {\"x\":1,}", "invoke \"a\" {\"x\":null}", "invoke \"a\" {\"x\":[]}", "invoke \"a\" {\"x\":1.5}",
            "invoke \"a\" {\"x\":01}", "invoke \"a\" {\"x\":9223372036854775808}", "invoke \"a\" {} extra", "invoke \"a\" {\"x\":\"\\u0000\"}"}) {
            QVERIFY2(!Compile(Bad, &Details).HasValue(), Bad); QVERIFY(Details.line_ > 0); QVERIFY(Details.column_ > 0);
        }
        QVERIFY(!Compile(QString(65537, ' ')).HasValue());
        QVERIFY(!Compile(QString("invoke \"a\" {}\n").repeated(257)).HasValue());
        QVERIFY(!Compile("# comment\nwrong", &Details).HasValue()); QCOMPARE(Details.line_, 2);
    }
    void acceptedAwaitAndPermissions() {
        auto Parsed = Compile("invoke \"hc.file.mkdir\" {\"destination\":\"child\"}\ninvoke \"hc.navigate.refresh\" {}"); QVERIFY(Parsed.HasValue());
        int Calls = 0, Polls = 0; bool Done = false, Cancelled = false, SourceCorrect = true;
        Host Host;
        Host.capture_ = []() -> Result<CommandContext> { return CommandContext{}; };
        Host.dispatch_ = [&](CommandInvocation Invocation) {
            ++Calls; SourceCorrect = SourceCorrect && Invocation.source_ == "automation";
            // Registry permission rejection is forwarded unchanged to the script.
            if (Invocation.id_ == "hc.file.mkdir" && !Invocation.permissions_.contains("file-write"))
                return Ready<CommandOutcome>(Error{ErrorCategory::AccessDenied, "permission-required"});
            return Ready<CommandOutcome>(CommandOutcome{Calls == 1 ? CommandOutcome::Status::Accepted : CommandOutcome::Status::Completed, Calls == 1 ? "job-1" : ""});
        };
        Host.poll_ = [&](const auto&, const auto&) -> Result<bool> { ++Polls; return Done; };
        Host.cancel_ = [&](const auto&, const auto&) { Cancelled = true; };
        Execution Denied(Parsed.Value(), Host, {}, "denied"); Denied.Tick(); Denied.Tick();
        QCOMPARE(Denied.Status(), State::Failed); QCOMPARE(Denied.Failure()->category_, ErrorCategory::AccessDenied);
        Calls = 0; Cancelled = false;
        Execution Run(Parsed.Value(), Host, {"file-write"}, "allowed"); Run.Tick(); Run.Tick(); Run.Tick();
        QCOMPARE(Calls, 1); QVERIFY(Polls > 0); QCOMPARE(Run.CompletedSteps(), 0u);
        Done = true; Run.Tick(); QCOMPARE(Calls, 2); Run.Tick(); QCOMPARE(Run.Status(), State::Completed); QCOMPARE(Run.CompletedSteps(), 2u); QVERIFY(!Cancelled); QVERIFY(SourceCorrect);
    }
    void cancellationDeadlineAndOwnerLoss() {
        auto Program = Compile("invoke \"hc.test\" {}").Value();
        std::promise<Result<CommandOutcome>> Promise; std::stop_token Observed; bool Cancelled = false;
        Host Host; Host.capture_ = []() -> Result<CommandContext> { return CommandContext{}; };
        Host.dispatch_ = [&](const CommandInvocation& Invocation) { Observed = Invocation.operation_.stopToken_; return Promise.get_future(); };
        Host.poll_ = [](const auto&, const auto&) -> Result<bool> { return false; };
        Host.cancel_ = [&](const auto&, const auto&) { Cancelled = true; };
        Execution Pending(Program, Host, {}, "pending"); Pending.Tick(); Pending.Cancel();
        QVERIFY(Observed.stop_requested()); QVERIFY(Cancelled); QCOMPARE(Pending.Status(), State::Cancelled);
        Promise.set_value(CommandOutcome{});
        OperationContext Context; Context.deadline_ = std::chrono::steady_clock::now();
        Execution Expired(Program, Host, {}, "expired", Context); Expired.Tick(); QCOMPARE(Expired.Failure()->code_, std::string("deadline-exceeded"));
        Host.capture_ = []() -> Result<CommandContext> { return Error{ErrorCategory::Cancelled, "tab-closed"}; };
        Execution Closed(Program, Host, {}, "closed"); Closed.Tick(); QCOMPARE(Closed.Failure()->code_, std::string("tab-closed"));
    }
    void dialogRunAndClose() {
        QWidget Parent; int Calls = 0; bool Cancelled = false;
        Host Host; Host.capture_ = []() -> Result<CommandContext> { return CommandContext{}; };
        Host.dispatch_ = [&](const auto&) { ++Calls; return Ready<CommandOutcome>(CommandOutcome{CommandOutcome::Status::Accepted, "pending"}); };
        Host.poll_ = [](const auto&, const auto&) -> Result<bool> { return false; };
        Host.cancel_ = [&](const auto&, const auto&) { Cancelled = true; };
        Show(&Parent, [Host] { return Host; }); auto* Dialog = Parent.findChild<QDialog*>("hc.automationDialog"); QVERIFY(Dialog);
        QVERIFY(!Dialog->findChild<QCheckBox*>("hc.automation.write")->isChecked());
        Dialog->findChild<QPushButton*>("hc.automation.run")->click(); QTRY_COMPARE(Calls, 1);
        QPointer<QDialog> Guard(Dialog); Dialog->close(); QTRY_VERIFY(Guard.isNull()); QVERIFY(Cancelled);
    }
};
QTEST_MAIN(AutomationTests)
#include "tstAutomation.moc"
