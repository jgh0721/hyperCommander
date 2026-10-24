#include "stdafx.h"

/// Product integration
#include "UniqueLibs/document/documentWebProcess.hpp"
#include "UniqueLibs/document/epubProcessView.hpp"
#include <QTimer>
#include "commonLibs/document/epub/epubPackage.hpp"
#include "commonLibs/document/epub/epubInputSender.hpp"

/// Platform/framework headers
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPlainTextEdit>
#include <QProcess>
#include "inputTransfer.hpp"
#include <QTabWidget>
#include <QtConcurrentRun>
#include <QtTest>

using namespace hc::document;
namespace
{
    QByteArray readFile(const QString& Path)
    { QFile File(Path); if(!File.open(QIODevice::ReadOnly)) return {}; return File.readAll(); }
    WebProcessRuntime runtime()
    {
        const auto ProductRoot = qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION");
        if(!ProductRoot.isEmpty()) {
            if(qEnvironmentVariableIsSet("HC_EPUB_USE_COMPILED_POLICY"))
                return MakeProductEpubRuntime(ProductRoot);
            const auto Pin = qgetenv("HC_EPUB_PRODUCT_MANIFEST_SHA256");
            const auto Manifest = readFile(ProductRoot + "/Libs/Epub/epub-runtime-inventory.json");
            if(Pin.size() != 64 || QCryptographicHash::hash(Manifest, QCryptographicHash::Sha256).toHex() != Pin) return {};
            return MakeProductEpubRuntime(ProductRoot, Pin);
        }
        WebProcessRuntime Runtime;
        Runtime.root_ = qEnvironmentVariable("HC_EPUB_POC_RUNTIME"); Runtime.kind_ = WebProcessKind::EpubPrototype;
        const auto Manifest = readFile(Runtime.root_ + "/poc-runtime.json");
        const auto Pin = qgetenv("HC_EPUB_POC_MANIFEST_SHA256");
        if(Pin.size() != 64 || QCryptographicHash::hash(Manifest, QCryptographicHash::Sha256).toHex() != Pin) return {};
        Runtime.approvedInventorySha256_ = Pin; return Runtime;
    }
    WebProcessLimits limits() { return {24, 2ULL * 1024 * 1024 * 1024, 20000}; }
    template<typename T> bool wait(QFuture<T>& Future)
    {
        QDeadlineTimer Deadline(60000);
        while(!Future.isFinished() && !Deadline.hasExpired()) QTest::qWait(20);
        return Future.isFinished();
    }
    struct Session
    {
        std::unique_ptr<DocumentWebProcess> owner, pending;
        WebProcessError error;
        bool Close()
        {
            auto Future = QtConcurrent::run([this] { return (!owner || owner->Close()) && (!pending || pending->Close()); });
            return wait(Future) && Future.result();
        }
        ~Session() { Close(); }
    };
    QJsonObject exchange(Session& Session, const QJsonObject& Request)
    {
        const auto Bytes = QJsonDocument(Request).toJson(QJsonDocument::Compact);
        auto Future = QtConcurrent::run([&Session, Bytes] {
            QByteArray Reply;
            if(!Session.owner->Exchange(Bytes, Reply, 5000, Session.error)) return QJsonObject{{"transportError", Session.error.code_}};
            return QJsonDocument::fromJson(Reply).object();
        });
        return wait(Future) ? Future.result() : QJsonObject{{"transportError", "test-timeout"}};
    }
    int jobProbe(const QString& Kind)
    {
        const auto Job = CreateJobObjectW(nullptr, nullptr);
        if(!Job) return 2;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION Info{};
        if(Kind == "single") { Info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_ACTIVE_PROCESS; Info.BasicLimitInformation.ActiveProcessLimit = 1; }
        if(!SetInformationJobObject(Job, JobObjectExtendedLimitInformation, &Info, sizeof(Info))
            || !AssignProcessToJobObject(Job, GetCurrentProcess())) return 3;
        if(Kind == "ui") {
            JOBOBJECT_BASIC_UI_RESTRICTIONS Ui{255};
            if(!SetInformationJobObject(Job, JobObjectBasicUIRestrictions, &Ui, sizeof(Ui))) return 4;
        }
        WebProcessError Error; auto Owner = DocumentWebProcess::Start(runtime(), limits(), 0, Error);
        const auto Expected = Kind == "ui" ? "web-owner-ui-restricted-caller" : "web-owner-parent-process-limit";
        const bool Ok = !Owner && Error.code_ == Expected;
        std::fprintf(stdout, "%s\n", qPrintable(Error.code_)); CloseHandle(Job); return Ok ? 0 : 5;
    }
}

class EpubOwnerTests final : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    { QVERIFY2(runtime().approvedInventorySha256_.has_value(), "Independent development runtime pin is required"); }
    void cleanup()
    {
        EpubProcessView::RequestShutdown();
        QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(), 30000);
        EpubProcessView::ReleaseLifecycle();
    }
    void productStandaloneDenied()
    {
        const auto Runtime = runtime();
        if(Runtime.kind_ != WebProcessKind::EpubReadOnly) QSKIP("Product helper only");
        QProcess Process;
        Process.start(Runtime.root_ + "/HyperEpubWeb.exe", {"--epub", "untrusted.epub"});
        QVERIFY(Process.waitForStarted(5000));
        QVERIFY(Process.waitForFinished(10000));
        QCOMPARE(Process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(Process.exitCode(), 2);
    }
    void viewerJobQualification()
    {
        const auto Output = qEnvironmentVariable("HC_EPUB_POC_EVIDENCE") + "/viewer-job";
        QVERIFY(QDir().mkpath(Output));
        QProcess Probe;
        Probe.start(QCoreApplication::applicationFilePath(), {"--qualification-viewer-job", Output});
        QVERIFY(Probe.waitForStarted(5000));
        QVERIFY(Probe.waitForFinished(60000));
        QCOMPARE(Probe.exitStatus(), QProcess::NormalExit);
        QVERIFY2(Probe.exitCode() == 0, readFile(Output + "/tests.txt").constData());
    }
    void resourceLimitFailure()
    {
        EpubProcessView Healthy(runtime(), limits()), Limited(runtime(), {12, 256ULL * 1024 * 1024, 5000});
        Healthy.resize(1000, 800); Limited.resize(1000, 800); Healthy.show(); Limited.show();
        QSignalSpy Ready(&Healthy, &EpubProcessView::Loaded), Failed(&Healthy, &EpubProcessView::Failed);
        QSignalSpy LimitedFailed(&Limited, &EpubProcessView::Failed), LimitedReady(&Limited, &EpubProcessView::Loaded);
        const auto Bytes = readFile(qEnvironmentVariable("HC_EPUB_POC_EPUB"));
        QVERIFY(Healthy.Load(Bytes));
        QTRY_VERIFY_WITH_TIMEOUT(Ready.count() || Failed.count(), 60000); QCOMPARE(Ready.count(), 1);
        QVERIFY(Limited.Load(Bytes));
        QTRY_COMPARE_WITH_TIMEOUT(LimitedFailed.count(), 1, 30000);
        QCOMPARE(LimitedReady.count(), 0);
        QVERIFY(Limited.ViewState()["message"] != QJsonValue("web-owner-limits"));
        QVERIFY(Limited.ViewState()["retired"].toBool());
        QCOMPARE(Healthy.ViewState()["status"], QJsonValue("ready"));
        QCOMPARE(Failed.count(), 0);
        Healthy.RequestClose(); QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(), 30000);
    }
    void startupVisibility_data()
    {
        QTest::addColumn<QString>("Mode");
        for(const auto* Mode : {"hidden-tab", "minimized"})
            QTest::newRow(Mode) << QString(Mode);
        for(int Run = 0; Run < 5; ++Run) QTest::newRow(qPrintable(QString("resize-%1").arg(Run))) << QString("resize");
    }
    void startupVisibility()
    {
        QFETCH(QString, Mode);
        QTabWidget Tabs; Tabs.resize(1000, 800);
        auto* Other = new QWidget; Tabs.addTab(Other, "Other");
        auto* View = new EpubProcessView(runtime(), limits()); Tabs.addTab(View, "EPUB");
        Tabs.setCurrentWidget(View); Tabs.show();
        QSignalSpy Ready(View, &EpubProcessView::Loaded), Failed(View, &EpubProcessView::Failed);
        QVERIFY(View->Load(readFile(qEnvironmentVariable("HC_EPUB_POC_EPUB"))));
        if(Mode == "hidden-tab") Tabs.setCurrentWidget(Other);
        if(Mode == "minimized") Tabs.showMinimized();
        for(int Index = 0; Index < 20; ++Index) {
            if(Mode == "resize") Tabs.resize(Index % 2 ? 1000 : 1250, 800);
            QTest::qWait(150);
        }
        Tabs.showNormal(); Tabs.setCurrentWidget(View);
        QTRY_VERIFY_WITH_TIMEOUT(Ready.count() || Failed.count(), 60000);
        QVERIFY2(Ready.count() == 1, QJsonDocument(View->ViewState()).toJson().constData());
        QCOMPARE(Failed.count(), 0);
        Tabs.resize(1350, 850);
        const auto Fits = [&] {
            const auto Startup = View->ViewState()["startup"].toObject();
            const auto Frames = Startup["frames"].toArray();
            return Frames.size() == 1 && Frames[0].toObject()["width"].toInt() > 0
                && qAbs(Frames[0].toObject()["width"].toInt() - Startup["paneWidth"].toInt()) <= 2;
        };
        QTRY_VERIFY_WITH_TIMEOUT(Fits(), 10000);
        View->RequestClose(); QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(), 30000);
    }
    void resourceQualification_data()
    {
        QTest::addColumn<int>("Run");
        QTest::newRow("first") << 0; QTest::newRow("repeat-1") << 1; QTest::newRow("repeat-2") << 2;
    }
    void resourceQualification()
    {
        QFETCH(int, Run);
        const WebProcessLimits Candidate{12, 1024ULL * 1024 * 1024, 20000};
        EpubProcessView First(runtime(), Candidate), Second(runtime(), Candidate);
        First.resize(900, 750); Second.resize(900, 750);
        First.move(0, 0); Second.move(950, 0); First.show(); Second.show();
        QSignalSpy FirstReady(&First, &EpubProcessView::Loaded), SecondReady(&Second, &EpubProcessView::Loaded);
        QSignalSpy FirstFailed(&First, &EpubProcessView::Failed), SecondFailed(&Second, &EpubProcessView::Failed);
        QVERIFY(First.Load(readFile(qEnvironmentVariable("HC_EPUB_POC_EPUB"))));
        QVERIFY(Second.Load(readFile(qEnvironmentVariable("HC_EPUB_POC_EPUB2")), 2));
        QTRY_VERIFY_WITH_TIMEOUT((FirstReady.count() && SecondReady.count()) || FirstFailed.count() || SecondFailed.count(), 60000);
        if(!FirstReady.count() || !SecondReady.count()) {
            const auto FirstState = First.ViewState(), SecondState = Second.ViewState();
            const auto Output = qEnvironmentVariable("HC_EPUB_POC_EVIDENCE");
            if(!Output.isEmpty()) {
                QFile File(Output + "/failure-" + QString::number(Run) + ".json");
                if(File.open(QIODevice::WriteOnly | QIODevice::NewOnly))
                    File.write(QJsonDocument(QJsonObject{{"first", FirstState}, {"second", SecondState}}).toJson());
            }
            QFAIL(QJsonDocument(QJsonObject{
                {"first", FirstState["lastReaderState"]}, {"second", SecondState["lastReaderState"]},
                {"firstError", FirstState["message"]}, {"secondError", SecondState["message"]}}).toJson().constData());
        }
        QTest::qWait(1000); // Sample steady display with both Jobs alive.
        First.RequestClose(); Second.RequestClose();
        QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(), 30000);
        QJsonArray Rows;
        for(auto* View : {&First, &Second}) {
            const auto Metrics = View->ViewState()["metrics"].toObject();
            QVERIFY(Metrics["readyMs"].toInteger(-1) >= 0);
            QVERIFY(Metrics["peakJobBytes"].toInteger() > 0);
            QVERIFY(Metrics["peakJobBytes"].toInteger() <= qint64(Candidate.memoryBytes_));
            QVERIFY(Metrics["peakObservedProcesses"].toInt() > 1);
            QVERIFY(Metrics["peakObservedProcesses"].toInt() <= int(Candidate.processes_));
            QCOMPARE(Metrics["activeProcesses"].toInt(-1), 0);
            Rows.append(Metrics);
        }
        const auto Output = qEnvironmentVariable("HC_EPUB_POC_EVIDENCE");
        QVERIFY(!Output.isEmpty());
        QFile File(Output + "/resources-" + QString::number(Run) + ".json");
        QVERIFY(File.open(QIODevice::WriteOnly | QIODevice::NewOnly));
        File.write(QJsonDocument(QJsonObject{{"schemaVersion", 1}, {"run", Run},
            {"concurrentBooks", 2}, {"rows", Rows},
            {"scope", "two separately positioned visible windows; synthetic EPUB2/3 fixtures; not occlusion, cold-cache or product F3/F4 qualification"}}).toJson());
    }
    void lifecycleCallbacks_data()
    {
        QTest::addColumn<QString>("Mode");
        for(const auto* Mode : {"ready", "cancel", "error", "delete-loaded", "delete-failed", "delete-closed", "focus"})
            QTest::newRow(Mode) << QString(Mode);
    }
    void lifecycleCallbacks()
    {
        QFETCH(QString, Mode);
        QTabWidget Tabs; Tabs.resize(1200, 900);
        auto* Other = new QPlainTextEdit; Tabs.addTab(Other, "Other");
        QPointer<EpubProcessView> View = new EpubProcessView(runtime(), limits());
        Tabs.addTab(View, "EPUB"); Tabs.setCurrentWidget(View); Tabs.show();
        QSignalSpy Loaded(View, &EpubProcessView::Loaded), Failed(View, &EpubProcessView::Failed), Closed(View, &EpubProcessView::Closed);
        QStringList Events;
        connect(View, &EpubProcessView::Loaded, this, [&] {
            QCOMPARE(QThread::currentThread(), qApp->thread()); Events << "loaded";
            if(Mode == "delete-loaded") delete View.data();
        });
        connect(View, &EpubProcessView::Failed, this, [&](const QString& Error) {
            QVERIFY(!Error.isEmpty()); QVERIFY(View->ViewState()["retired"].toBool()); Events << "failed";
            if(Mode == "delete-failed") delete View.data();
        });
        connect(View, &EpubProcessView::Closed, this, [&] {
            QVERIFY(View->ViewState()["retired"].toBool()); Events << "closed";
            if(Mode == "delete-closed") delete View.data();
        });
        const bool Bad = Mode == "error" || Mode == "delete-failed";
        QVERIFY(View->Load(Bad ? QByteArray("invalid ZIP") : readFile(qEnvironmentVariable("HC_EPUB_POC_EPUB"))));
        if(Mode == "cancel" || Mode == "delete-closed") View->RequestClose();
        else if(Mode == "focus") {
            QVERIFY(View->FocusInput());
            Tabs.setCurrentWidget(Other); QVERIFY(!View->FocusInput());
        }
        if(!Bad && Mode != "cancel" && Mode != "delete-closed") {
            QTRY_VERIFY_WITH_TIMEOUT(Loaded.count() || Failed.count(), 60000);
            QCOMPARE(Loaded.count(), 1);
            if(Mode == "focus") {
                QVERIFY(!View->ViewState()["focusAcknowledged"].toBool());
                Tabs.setCurrentWidget(View); Tabs.activateWindow();
                QVERIFY(QTest::qWaitForWindowActive(&Tabs));
                QVERIFY(View->FocusInput());
                QTRY_VERIFY_WITH_TIMEOUT(View->ViewState()["focusAcknowledged"].toBool(), 10000);
                QTRY_VERIFY_WITH_TIMEOUT(View->ViewState()["inputFocused"].toBool(), 10000);
            }
            if(View) View->RequestClose();
        }
        if(Mode == "delete-failed") { QTRY_VERIFY_WITH_TIMEOUT(View.isNull(), 30000); }
        else if(View) {
            QTRY_COMPARE_WITH_TIMEOUT(Closed.count(), 1, 30000);
            if(View) { QVERIFY(!View->FocusInput()); View->RequestClose(); }
        }
        QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(), 30000);
        QCOMPARE(Failed.count(), Bad ? 1 : 0);
        if(Bad && Mode != "delete-failed") QCOMPARE(Events, QStringList({"failed", "closed"}));
        if(Mode == "cancel" || Mode == "delete-closed") QCOMPARE(Loaded.count(), 0);
        if(Mode.startsWith("delete-")) QVERIFY(View.isNull());
    }
    void asyncAdmission()
    {
        EpubProcessView WrongKind({}, limits());
        QVERIFY(!WrongKind.Load("bytes"));
        EpubProcessView Empty(runtime(), limits());
        QVERIFY(!Empty.Load({}));
        EpubProcessView WrongGeneration(runtime(), limits());
        QVERIFY(!WrongGeneration.Load("bytes", 0));
        QVERIFY(EpubProcessView::IsDrained());
    }
    void asyncShutdown()
    {
        EpubProcessView::InitializeLifecycle();
        EpubProcessView First(runtime(), limits()), Second(runtime(), limits());
        First.resize(1000, 800); Second.resize(1000, 800); First.show(); Second.show();
        const auto Bytes = readFile(qEnvironmentVariable("HC_EPUB_POC_EPUB"));
        QVERIFY(First.Load(Bytes)); QVERIFY(Second.Load(Bytes));
        EpubProcessView::RequestShutdown();
        QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(), 30000);
        QVERIFY(First.ViewState()["retired"].toBool()); QVERIFY(Second.ViewState()["retired"].toBool());
        EpubProcessView Rejected(runtime(), limits());
        QVERIFY(!Rejected.Load(Bytes));
        QCOMPARE(Rejected.ViewState()["message"].toString(), QString("epub-native-binding-unavailable"));
    }
    void asyncView_data()
    {
        QTest::addColumn<QString>("Mode");
        for(const auto* Mode : {"ready-close", "destroy-startup", "destroy-attached", "destroy-ready", "crash", "bad-input", "bad-pin"})
            QTest::newRow(Mode) << QString(Mode);
    }
    void asyncView()
    {
        QFETCH(QString, Mode);
        EpubProcessView::InitializeLifecycle();
        QTabWidget Tabs;
        auto* Other = new QPlainTextEdit;
        Other->setPlainText(QStringLiteral("Unsaved other tab")); Other->document()->setModified(true);
        Tabs.addTab(Other, "Other");
        auto Runtime = runtime();
        if(Mode == "bad-pin") Runtime.approvedInventorySha256_ = QByteArray(64, '0');
        auto* View = new EpubProcessView(Runtime, limits());
        Tabs.addTab(View, "EPUB"); Tabs.setCurrentWidget(View); Tabs.resize(1200, 800); Tabs.show();
        int Heartbeats = 0;
        QTimer Heartbeat; Heartbeat.setInterval(10);
        connect(&Heartbeat, &QTimer::timeout, this, [&] { ++Heartbeats; }); Heartbeat.start();
        const auto Bytes = Mode == "bad-input" ? QByteArray("invalid ZIP") : readFile(qEnvironmentVariable("HC_EPUB_POC_EPUB"));
        QVERIFY(!Bytes.isEmpty());
        QVERIFY(View->Load(Bytes, 42));
        QVERIFY(!View->Load(Bytes, 43));
        if(Mode == "destroy-startup") {
            delete View; View = nullptr;
        } else if(Mode == "bad-input" || Mode == "bad-pin") {
            QTRY_VERIFY_WITH_TIMEOUT(View->ViewState()["retired"].toBool(), 30000);
            QCOMPARE(View->ViewState()["status"].toString(), QString("error"));
            QVERIFY(!View->ViewState()["message"].toString().isEmpty());
        } else if(Mode == "destroy-attached") {
            QTRY_VERIFY_WITH_TIMEOUT(View->ViewState()["attached"].toBool() || View->ViewState()["retired"].toBool(), 30000);
            QVERIFY2(View->ViewState()["attached"].toBool(), QJsonDocument(View->ViewState()).toJson().constData());
            delete View; View = nullptr;
        } else {
            QTRY_VERIFY_WITH_TIMEOUT(View->ViewState()["status"] == "ready" || View->ViewState()["retired"].toBool(), 60000);
            QVERIFY2(View->ViewState()["status"] == "ready", QJsonDocument(View->ViewState()).toJson().constData());
            QVERIFY(View->ViewState()["attached"].toBool());
            QVERIFY(View->ViewState()["text"].toString().contains(QStringLiteral("나무")));
            QVERIFY(!View->ViewState()["images"].toArray().isEmpty());
            QVERIFY(Heartbeats > 5);
            if(Mode == "destroy-ready") { delete View; View = nullptr; }
            else if(Mode == "crash") {
                const auto Process = OpenProcess(PROCESS_TERMINATE, FALSE, View->ViewState()["processId"].toInt());
                QVERIFY(Process); const bool Killed = TerminateProcess(Process, 99); CloseHandle(Process); QVERIFY(Killed);
                QTRY_VERIFY_WITH_TIMEOUT(View->ViewState()["retired"].toBool(), 30000);
                QCOMPARE(View->ViewState()["status"].toString(), QString("error"));
            } else {
                View->RequestClose();
                QTRY_VERIFY_WITH_TIMEOUT(View->ViewState()["retired"].toBool(), 30000);
                QCOMPARE(View->ViewState()["status"].toString(), QString("closed"));
                QCOMPARE(View->ViewState()["activeProcessesAfterClose"].toInt(-1), 0);
            }
        }
        QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(), 30000);
        QCOMPARE(Other->toPlainText(), QString("Unsaved other tab"));
        QVERIFY(Other->document()->isModified());
    }
    void descriptorAndTamperRejection()
    {
        auto Runtime = runtime(); WebProcessError Error;
        Runtime.approvedInventorySha256_ = QByteArray(64, '0');
        QVERIFY(!DocumentWebProcess::Start(Runtime, limits(), 0, Error)); QCOMPARE(Error.code_, QString("epub-runtime-inventory-hash"));
        Runtime = runtime(); Runtime.sha256_["bundle/reader.js"] = QByteArray(64, '0');
        QVERIFY(!DocumentWebProcess::Start(Runtime, limits(), 0, Error)); QCOMPARE(Error.code_, QString("epub-runtime-mixed-authority"));
        Runtime = runtime(); Runtime.kind_ = WebProcessKind::Rhwp;
        QVERIFY(!DocumentWebProcess::Start(Runtime, limits(), 0, Error)); QCOMPARE(Error.code_, QString("rhwp-runtime-inventory-missing"));
    }
    void startupCancellation()
    {
        WebProcessError Error; std::unique_ptr<DocumentWebProcess> Pending; int Checks = 0;
        QVERIFY(!DocumentWebProcess::Start(runtime(), limits(), 0, Error, &Pending, [&] { return ++Checks > 2; }));
        QVERIFY(!Pending); QCOMPARE(Error.code_, QString("web-owner-runtime-cancelled"));
    }
    void restrictedParent_data()
    { QTest::addColumn<QString>("Kind"); QTest::newRow("viewer-ui-job") << QString("ui"); QTest::newRow("legacy-one-process-job") << QString("single"); }
    void restrictedParent()
    {
        QFETCH(QString, Kind); QProcess Probe;
        Probe.start(QCoreApplication::applicationFilePath(), {"--job-probe", Kind});
        QVERIFY(Probe.waitForStarted(5000)); QVERIFY(Probe.waitForFinished(10000));
        QCOMPARE(Probe.exitStatus(), QProcess::NormalExit); QVERIFY2(Probe.exitCode() == 0, Probe.readAllStandardOutput().constData());
    }
    void nativeTabLifecycle_data()
    {
        QTest::addColumn<QString>("Engine"); QTest::addColumn<QString>("Retirement"); QTest::addColumn<bool>("SecondBook");
        QTest::newRow("epubjs") << QString("epubjs") << QString("normal") << false;
        QTest::newRow("epubjs-other-book") << QString("epubjs") << QString("normal") << true;
        QTest::newRow("readium") << QString("readium") << QString("normal") << false;
        QTest::newRow("readium-other-book") << QString("readium") << QString("normal") << true;
        QTest::newRow("close-after-ready") << QString("epubjs") << QString("early") << false;
        QTest::newRow("sender-cancel") << QString("epubjs") << QString("sender-cancel") << false;
        QTest::newRow("cancel-transfer") << QString("epubjs") << QString("cancel") << false;
        QTest::newRow("bad-publication") << QString("epubjs") << QString("invalid") << false;
        QTest::newRow("source-identity-mismatch") << QString("epubjs") << QString("source-mismatch") << false;
        QTest::newRow("resource-hash-mismatch") << QString("epubjs") << QString("resource-mismatch") << false;
        QTest::newRow("renderer-owner-crash") << QString("epubjs") << QString("crash") << false;
    }
    void nativeTabLifecycle()
    {
        QFETCH(QString, Engine); QFETCH(QString, Retirement); QFETCH(bool, SecondBook);
        QTabWidget Tabs; Tabs.setAttribute(Qt::WA_DontShowOnScreen); Tabs.resize(1280, 930);
        auto* Other = new QPlainTextEdit; Other->setPlainText(QStringLiteral("다른 탭의 미저장 본문")); Other->document()->setModified(true);
        Tabs.addTab(Other, "Existing text"); auto* Parent = new QWidget; Parent->setAttribute(Qt::WA_NativeWindow);
        Tabs.addTab(Parent, "EPUB"); Tabs.setCurrentIndex(1); Tabs.show();
        const auto Handle = static_cast<std::uintptr_t>(Parent->winId());
        Session Session; const auto Runtime = runtime();
        auto Start = QtConcurrent::run([&Session, Runtime, Handle] { Session.owner = DocumentWebProcess::Start(Runtime, limits(), Handle, Session.error, &Session.pending); });
        QVERIFY(wait(Start)); QVERIFY2(Session.owner, qPrintable(Session.error.code_ + ":" + QString::number(Session.error.native_)));
        const auto Snapshot = Session.owner->Snapshot(); QVERIFY(Snapshot.authenticated_); QVERIFY(Snapshot.running_);
        QCOMPARE(GetParent(reinterpret_cast<HWND>(Snapshot.childWindow_)), reinterpret_cast<HWND>(Handle));
        if(Retirement == "early") {
            Tabs.removeTab(1); Parent->hide(); QVERIFY(Session.Close());
            QCOMPARE(Session.owner->Snapshot().activeProcesses_, 0U); QVERIFY(!IsWindow(reinterpret_cast<HWND>(Snapshot.childWindow_)));
            delete Parent; QCOMPARE(Tabs.count(), 1); QVERIFY(Other->document()->isModified()); return;
        }
        if(Runtime.kind_ == WebProcessKind::EpubReadOnly && Engine == "readium") {
            QJsonObject Begin{{"kind", "input-begin"}, {"engine", "readium"}};
            QCOMPARE(exchange(Session, Begin)["error"], QJsonValue("epub-product-engine"));
            QVERIFY(Session.Close()); QCOMPARE(Session.owner->Snapshot().activeProcesses_, 0U);
            return;
        }
        QByteArray PublicationBytes;
        const auto EpubPath = qEnvironmentVariable(SecondBook ? "HC_EPUB_POC_EPUB2" : "HC_EPUB_POC_EPUB");
        if(!EpubPath.isEmpty()) {
            auto Preparing = QtConcurrent::run([EpubPath] { return hc::document::epub::PrepareFile(EpubPath); });
            QVERIFY(wait(Preparing));
            const auto Prepared = Preparing.result(); QVERIFY2(Prepared, qPrintable(Prepared.error_));
            PublicationBytes = QJsonDocument(Prepared.publication_).toJson(QJsonDocument::Compact);
        } else PublicationBytes = readFile(qEnvironmentVariable(SecondBook ? "HC_EPUB_POC_PUBLICATION2" : "HC_EPUB_POC_PUBLICATION"));
        const auto Publication = QJsonDocument::fromJson(PublicationBytes).object(); QVERIFY(!Publication.isEmpty());
        QJsonObject Request{{"protocolVersion", 1}, {"generation", SecondBook ? 7 : 1}, {"documentId", Publication["sourceSha256"]}, {"kind", "snapshot"}};
        auto Payload = PublicationBytes;
        if(Retirement == "invalid") Payload = "{invalid";
        else if(Retirement == "source-mismatch" || Retirement == "resource-mismatch") {
            auto Changed = Publication;
            if(Retirement == "source-mismatch") Changed["sourceSha256"] = QString(64, '0');
            else {
                auto Resources = Changed["resources"].toArray(); auto Resource = Resources[0].toObject();
                Resource["sha256"] = QString(64, '0'); Resources[0] = Resource; Changed["resources"] = Resources;
            }
            Payload = QJsonDocument(Changed).toJson(QJsonDocument::Compact);
        }
        // JSON trailing whitespace exercises more than one real IPC frame without changing book resources.
        Payload += QByteArray(hc::epub::poc::InputTransfer::ChunkBytes + 13, ' ');
        if(Retirement != "cancel") {
            auto Sending = QtConcurrent::run([&] {
                bool CancelTransfer = false;
                return hc::document::epub::SendInput(Payload, Request["documentId"].toString(), Request["generation"].toInteger(), Engine,
                    [&](const QByteArray& Bytes, QByteArray& Reply, int Timeout) {
                        const bool Ok = Session.owner->Exchange(Bytes, Reply, Timeout, Session.error);
                        if(Retirement == "sender-cancel" && QJsonDocument::fromJson(Bytes).object()["kind"] == "input-chunk") CancelTransfer = true;
                        return Ok;
                    }, [&] { return CancelTransfer; });
            });
            QVERIFY(wait(Sending)); const auto Sent = Sending.result();
            if(Retirement == "sender-cancel") {
                QVERIFY(!Sent.accepted_); QVERIFY(Sent.retireRequired_); QCOMPARE(Sent.error_, QString("epub-send-cancelled"));
                QCOMPARE(exchange(Session, Request)["state"].toObject()["status"], QJsonValue("cancelled"));
                QVERIFY(Session.Close()); QCOMPARE(Session.owner->Snapshot().activeProcesses_, 0U); return;
            }
            QVERIFY2(Sent.accepted_, qPrintable(Sent.error_)); QVERIFY(!Sent.retireRequired_);
        } else {
        auto Begin = Request; Begin["kind"] = "input-begin"; Begin["byteLength"] = Payload.size(); Begin["readOnly"] = true; Begin["engine"] = Engine;
        Begin["sha256"] = QString::fromLatin1(QCryptographicHash::hash(Payload, QCryptographicHash::Sha256).toHex());
        QVERIFY(exchange(Session, Begin)["accepted"].toBool());
        for(qsizetype Offset = 0; Offset < Payload.size(); Offset += hc::epub::poc::InputTransfer::ChunkBytes) {
            auto Chunk = Request; Chunk["kind"] = "input-chunk"; Chunk["offset"] = Offset;
            Chunk["data"] = QString::fromLatin1(Payload.mid(Offset, hc::epub::poc::InputTransfer::ChunkBytes).toBase64());
            QVERIFY(exchange(Session, Chunk)["accepted"].toBool());
            if(Retirement == "cancel") {
                auto Cancel = Request; Cancel["kind"] = "input-cancel";
                QCOMPARE(exchange(Session, Cancel)["status"], QJsonValue("cancelled"));
                auto Commit = Request; Commit["kind"] = "input-commit"; QVERIFY(exchange(Session, Commit).contains("error"));
                QCOMPARE(exchange(Session, Request)["state"].toObject()["status"], QJsonValue("cancelled"));
                QVERIFY(Session.Close()); QCOMPARE(Session.owner->Snapshot().activeProcesses_, 0U); return;
            }
        }
        auto Commit = Request; Commit["kind"] = "input-commit"; QVERIFY(exchange(Session, Commit)["accepted"].toBool());
        }
        QJsonObject State; QDeadlineTimer Deadline(30000);
        do { const auto Reply = exchange(Session, Request); QVERIFY2(Reply.contains("state"), QJsonDocument(Reply).toJson().constData()); State = Reply["state"].toObject(); QTest::qWait(100); }
        while(State["status"] != "ready" && State["status"] != "error" && !Deadline.hasExpired());
        if(Retirement == "invalid" || Retirement == "source-mismatch" || Retirement == "resource-mismatch") {
            const auto Expected = Retirement == "invalid" ? "epub-publication-json" : Retirement == "source-mismatch"
                ? "epub-publication-identity" : "Resource hash or budget mismatch";
            QCOMPARE(State["status"], QJsonValue("error")); QCOMPARE(State["message"], QJsonValue(Expected));
            QVERIFY(Session.Close()); QCOMPARE(Session.owner->Snapshot().activeProcesses_, 0U); return;
        }
        QVERIFY2(State["status"] == "ready", QJsonDocument(State).toJson().constData());
        auto Bad = Request; Bad["generation"] = 2;
        QCOMPARE(exchange(Session, Bad)["error"].toString(), QString("epub-control-identity"));
        Bad = Request; Bad["kind"] = "open-file"; Bad["path"] = "C:/outside.epub";
        QCOMPARE(exchange(Session, Bad)["error"].toString(), QString("epub-control-command"));
        auto StartRequest = Request; StartRequest["kind"] = "start"; StartRequest["engine"] = Engine;
        QVERIFY(exchange(Session, StartRequest)["accepted"].toBool());
        Deadline = QDeadlineTimer(30000);
        do { const auto Reply = exchange(Session, Request); QVERIFY2(Reply.contains("state"), QJsonDocument(Reply).toJson().constData()); State = Reply["state"].toObject(); QTest::qWait(100); }
        while(State["status"] != "ready" && State["status"] != "error" && !Deadline.hasExpired());
        QVERIFY2(State["status"] == "ready", QJsonDocument(State).toJson().constData());
        QVERIFY(State["text"].toString().contains(QStringLiteral("나무"))); QVERIFY(!State["images"].toArray().isEmpty());
        Tabs.setCurrentIndex(0); QTest::qWait(100); QCOMPARE(Other->toPlainText(), QStringLiteral("다른 탭의 미저장 본문")); QVERIFY(Other->document()->isModified());
        auto HiddenFocus = Request; HiddenFocus["kind"] = "focus";
        QCOMPARE(exchange(Session, HiddenFocus)["focusRequested"], QJsonValue(false));
        Tabs.setCurrentIndex(1); Tabs.resize(1400, 950); QTest::qWait(200);
        RECT Rect{}; QVERIFY(GetClientRect(reinterpret_cast<HWND>(Snapshot.childWindow_), &Rect));
        RECT ParentRect{}; QVERIFY(GetClientRect(reinterpret_cast<HWND>(Handle), &ParentRect)); QCOMPARE(Rect.right, ParentRect.right);
        auto Test = Request; Test["kind"] = "test"; QVERIFY(exchange(Session, Test)["accepted"].toBool());
        Deadline = QDeadlineTimer(30000);
        do { const auto Reply = exchange(Session, Request); QVERIFY2(Reply.contains("state"), QJsonDocument(Reply).toJson().constData()); State = Reply["state"].toObject(); QTest::qWait(100); }
        while(State["status"] != "passed" && State["status"] != "failed" && State["status"] != "error" && !Deadline.hasExpired());
        QVERIFY2(State["status"] == "passed", QJsonDocument(State).toJson().constData()); QCOMPARE(State["checks"].toObject().size(), 16);
        if(Retirement == "crash") {
            const auto Process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, Snapshot.processId_); QVERIFY(Process);
            const bool Terminated = TerminateProcess(Process, 99); const auto Ended = WaitForSingleObject(Process, 5000); CloseHandle(Process);
            QVERIFY(Terminated); QCOMPARE(Ended, DWORD(WAIT_OBJECT_0));
            QVERIFY(exchange(Session, Request).contains("transportError"));
        }
        Tabs.removeTab(1); Parent->hide(); QVERIFY(Session.Close());
        const auto Closed = Session.owner->Snapshot(); QVERIFY(!Closed.running_); QCOMPARE(Closed.activeProcesses_, 0U);
        QVERIFY(!IsWindow(reinterpret_cast<HWND>(Snapshot.childWindow_))); delete Parent;
        QCOMPARE(Tabs.count(), 1); QVERIFY(Other->document()->isModified());
        const auto Output = qEnvironmentVariable("HC_EPUB_POC_EVIDENCE");
        if(!Output.isEmpty()) {
            QFile File(Output + "/" + Engine + "-" + Retirement + (SecondBook ? "-book2" : "") + "-owner.json"); QVERIFY(File.open(QIODevice::WriteOnly | QIODevice::NewOnly));
            State["payloadBytes"] = Payload.size(); State["transferChunks"] = (Payload.size() + hc::epub::poc::InputTransfer::ChunkBytes - 1) / hc::epub::poc::InputTransfer::ChunkBytes;
            State["sourceSha256"] = Publication["sourceSha256"]; State["generation"] = Request["generation"];
            State["authenticated"] = Snapshot.authenticated_; State["processId"] = int(Snapshot.processId_);
            State["activeProcessesAfterClose"] = int(Closed.activeProcesses_); State["otherTabPreserved"] = true;
            File.write(QJsonDocument(State).toJson());
        }
    }
};
int main(int Argc, char** Argv)
{
    if(Argc == 3 && QByteArray(Argv[1]) == "--job-probe") { QCoreApplication App(Argc, Argv); return jobProbe(QString::fromLocal8Bit(Argv[2])); }
    if(Argc == 3 && QByteArray(Argv[1]) == "--qualification-viewer-job") {
        // Mirrors the reviewed built-in viewer Job, not an actual F3 launch.
        // Keep this handle until process exit: closing a kill-on-close Job here
        // would terminate the test process itself.
        const auto Job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION Info{};
        Info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
        if(!Job || !SetInformationJobObject(Job, JobObjectExtendedLimitInformation, &Info, sizeof(Info))
            || !AssignProcessToJobObject(Job, GetCurrentProcess())) return 20;
        const auto Output = QString::fromLocal8Bit(Argv[2]);
        qputenv("HC_EPUB_POC_EVIDENCE", Output.toUtf8());
        QApplication App(Argc, Argv); App.setQuitOnLastWindowClosed(false); EpubOwnerTests Tests;
        return QTest::qExec(&Tests, QStringList{QString::fromLocal8Bit(Argv[0]), "resourceQualification",
            "-o", Output + "/tests.txt,txt"});
    }
    QApplication App(Argc, Argv); App.setQuitOnLastWindowClosed(false); EpubOwnerTests Tests; return QTest::qExec(&Tests, Argc, Argv);
}
#include "testOwner.moc"
