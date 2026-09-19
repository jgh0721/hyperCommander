#include "UniqueLibs/qtAutomation.hpp"
#include <QtTest>
#include <QDialog>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QLabel>
#include <QCheckBox>
#include <QSettings>
#include <QTemporaryDir>
#include <QMainWindow>
#include <QAction>
#include <QToolBar>
#include <QLineEdit>
#include <QKeySequenceEdit>

using namespace hc::automation;
class AutomationTests : public QObject {
    Q_OBJECT
private slots:
    void savedLibraryVersionAndConcurrentChange() {
        QTemporaryDir Temp; QVERIFY(Temp.isValid()); QSettings Settings(Temp.filePath("scripts.ini"),QSettings::IniFormat);
        LibraryStore First(Settings), Second(Settings); QVERIFY(First.Load().HasValue()); QVERIFY(Second.Load().HasValue());
        QList<SavedScript> Scripts{{"one","테스트","invoke \"test\" {}","Ctrl+Alt+9",true}};
        QVERIFY(First.Save(Scripts).HasValue()); QVERIFY(!Second.Save({}).HasValue());
        auto Loaded=Second.Load(); QVERIFY(Loaded.HasValue()); QCOMPARE(Loaded.Value()[0].title_,QString("테스트")); QVERIFY(Loaded.Value()[0].toolbar_);
        Scripts.append({"two","duplicate","invoke \"test\" {}","Ctrl+Alt+9",false}); QVERIFY(!EncodeLibrary(Scripts).HasValue());
        QVERIFY(DecodeLibrary("{\"version\":0,\"scripts\":[{\"id\":\"old\",\"title\":\"old\",\"source\":\"invoke \\\"test\\\" {}\"}]}").HasValue());
        const QByteArray Future="{\"version\":2,\"scripts\":[]}"; Settings.setValue("Automation/library",Future); Settings.sync();
        QVERIFY(!First.Load().HasValue()); QVERIFY(!First.Save({}).HasValue()); QCOMPARE(Settings.value("Automation/library").toByteArray(),Future);
    }
    void savedActionsAndWritePreflight() {
        QTemporaryDir Temp; QSettings Settings(Temp.filePath("actions.ini"),QSettings::IniFormat); LibraryStore Store(Settings); QVERIFY(Store.Load().HasValue());
        QVERIFY(Store.Save({{"read","읽기","invoke \"read\" {}","Ctrl+Alt+9",true},{"write","쓰기","invoke \"write\" {}","",false}}).HasValue());
        int Calls=0; Host Host; Host.capture_=[]() -> Result<CommandContext> { return CommandContext{}; };
        Host.schema_=[](const std::string& Id) -> Result<CommandDescriptor> { return CommandDescriptor{Id,1,{},Id=="write" ? "file-write":"navigation"}; };
        Host.dispatch_=[&](const auto&) { ++Calls; return Ready<CommandOutcome>(CommandOutcome{}); };
        Host.poll_=[](const auto&,const auto&) -> Result<bool> { return true; }; Host.cancel_=[](const auto&,const auto&) {};
        QMainWindow Window; Install(&Window,Settings,[Host] { return Host; }); Window.show();
        auto* Read=Window.findChild<QAction*>("hc.script.read"); auto* Write=Window.findChild<QAction*>("hc.script.write"); QVERIFY(Read); QVERIFY(Write);
        auto* Toolbar=Window.findChild<QToolBar*>("hc.automationToolbar"); QVERIFY(Toolbar->actions().contains(Read)); QVERIFY(!Toolbar->actions().contains(Write));
        QCOMPARE(Read->shortcut(),QKeySequence("Ctrl+Alt+9")); Read->trigger(); QTRY_COMPARE(Calls,1);
        QPointer<QDialog> Dialog=Window.findChild<QDialog*>("hc.automationDialog"); QVERIFY(Dialog); Dialog->close(); QTRY_VERIFY(Dialog.isNull());
        Write->trigger(); Dialog=Window.findChild<QDialog*>("hc.automationDialog"); QVERIFY(Dialog);
        QTRY_VERIFY(Dialog->findChild<QLabel*>("hc.automation.status")->text().contains("permission-required")); QCOMPARE(Calls,1);
        Dialog->findChild<QCheckBox*>("hc.automation.write")->setChecked(true); Dialog->findChild<QPushButton*>("hc.automation.run")->click(); QTRY_COMPARE(Calls,2);
        Dialog->close(); QTRY_VERIFY(Dialog.isNull());
        // New scripts must not shadow an existing application shortcut.
        auto* Existing=new QAction(&Window); Existing->setShortcut(QKeySequence("Ctrl+Alt+8")); Window.addAction(Existing);
        Window.findChild<QAction*>("hc.menu.automation")->trigger(); Dialog=Window.findChild<QDialog*>("hc.automationDialog");
        Dialog->findChild<QLineEdit*>("hc.automation.title")->setText("collision");
        Dialog->findChild<QKeySequenceEdit*>("hc.automation.shortcut")->setKeySequence(Existing->shortcut());
        Dialog->findChild<QPushButton*>("hc.automation.save")->click(); QVERIFY(Dialog->findChild<QLabel*>("hc.automation.status")->text().contains("단축키"));
        QCOMPARE(Store.Load().Value().size(),2); Dialog->close(); QTRY_VERIFY(Dialog.isNull());
    }
    void expressionsBranchesAndPipeline() {
        auto Parsed = Compile("version 1\nlet dest = \"한글\"\nif false && $missing\ninvoke \"never\" {}\nelse\ninvoke \"first\" {\"value\":$dest} as first |> invoke \"second\" {\"value\":$result.path}\nend\nif !false && (2 < 3 || $missing)\ninvoke \"third\" {\"value\":$first.path}\nend");
        QVERIFY(Parsed.HasValue()); QStringList Calls;
        Host Host; Host.capture_ = []() -> Result<CommandContext> { return CommandContext{}; };
        Host.dispatch_ = [&](const CommandInvocation& Invocation) { Calls.append(QString::fromStdString(Invocation.id_));
            if (std::get<std::string>(Invocation.parameters_.at("value")) != "한글") return Ready<CommandOutcome>(Error{ErrorCategory::Internal,"wrong-value"});
            return Ready<CommandOutcome>(CommandOutcome{}); };
        Host.output_ = [](const auto&, const auto&) -> Result<Parameters> { return Parameters{{"path",std::string("한글")}}; };
        Host.poll_ = [](const auto&,const auto&) -> Result<bool> { return true; }; Host.cancel_ = [](const auto&,const auto&) {};
        Execution Run(Parsed.Value(),Host,{},"expressions"); for (int I=0;I<20 && Run.Status()==State::Running;++I) Run.Tick();
        QCOMPARE(Run.Status(),State::Completed); QCOMPARE(Calls,QStringList({"first","second","third"})); QCOMPARE(Run.CompletedSteps(),3u);
        for (const auto& Text : {"if true\ninvoke \"a\" {}", "else", "end", "version 2\ninvoke \"a\" {}", "let result = 1", "invoke \"a\" {} |>"}) QVERIFY(!Compile(Text).HasValue());
    }
    void failurePoliciesAndSchema() {
        Host Host; Host.capture_ = []() -> Result<CommandContext> { return CommandContext{}; };
        int Calls=0; Host.dispatch_ = [&](const CommandInvocation& Invocation) { ++Calls;
            return Invocation.id_ == "missing" ? Ready<CommandOutcome>(Error{ErrorCategory::NotFound,"missing"}) : Ready<CommandOutcome>(CommandOutcome{}); };
        Host.poll_=[](const auto&,const auto&) -> Result<bool> { return true; }; Host.cancel_=[](const auto&,const auto&) {};
        auto Program = Compile("onerror continue\ninvoke \"missing\" {} |> invoke \"skip\" {}\nif !$result.ok\ninvoke \"recover\" {}\nend"); QVERIFY(Program.HasValue());
        Execution Run(Program.Value(),Host,{},"continue"); for(int I=0;I<20 && Run.Status()==State::Running;++I) Run.Tick();
        QCOMPARE(Run.Status(),State::CompletedWithErrors); QCOMPARE(Calls,2); QCOMPARE(Run.Errors().size(),1u);
        SchemaLookup Schema=[](const auto&) -> Result<CommandDescriptor> { return CommandDescriptor{"typed",1,{{"path",{ParameterType::String}}}}; };
        QVERIFY(!Compile("invoke \"typed\" {}",nullptr,Schema).HasValue());
        QVERIFY(!Compile("invoke \"typed\" {\"path\":1}",nullptr,Schema).HasValue());
        QVERIFY(Compile("invoke \"typed\" {\"path\":$context.path}",nullptr,Schema).HasValue());
        Host.dispatch_=[](const auto&) { return Ready<CommandOutcome>(Error{ErrorCategory::AccessDenied,"denied"}); };
        Execution Denied(Program.Value(),Host,{},"denied-continue"); Denied.Tick(); Denied.Tick(); QCOMPARE(Denied.Status(),State::Failed);
    }
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
