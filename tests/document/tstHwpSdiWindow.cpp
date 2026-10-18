#include "UniqueLibs/document/hwpSdiWindow.hpp"
#include <QtTest>
#include "UniqueLibs/document/hwpOpenWindow.hpp"
#include "UniqueLibs/document/hwpPrintBridge.hpp"
#include "UniqueLibs/document/hwpHostSession.hpp"
#include <QTemporaryDir>
#include <QFile>
#include <QDirIterator>
#include <QTimer>
#include <QLabel>
#include <QPointer>
#include <QScopeGuard>
#include <QMenuBar>
#include <QToolBar>
#include <QTabWidget>
#include <QPlainTextEdit>
#include <QMessageBox>
#include <QFileDialog>
#include <QPrinter>
#include <QPageSize>
#include <QPageLayout>
#include <QAbstractButton>
#include "UniqueLibs/document/documentRecoveryStore.hpp"
#include <QCryptographicHash>
#include <QJsonArray>
using namespace hc::document;

class FakeBackend final : public HwpSdiBackend
{
public:
    quint64 binding_ = 0;
    DocumentOperationTicket ticket_;
    int loads_ = 0, cancels_ = 0, stops_ = 0;
    bool editorEnabled_ = false;
    QJsonObject command_;
    bool holdSync_ = false;
    bool EditorCommand(const QJsonObject& Command) override {
        command_ = Command;
        if (!editorEnabled_) return false;
        const auto Type = Command["type"].toString();
        if (Type == "DocumentEditorSynchronize" && !holdSync_) {
            emit EditorReply(binding_, {{"editor", QJsonObject{{"synchronized", Command["id"]}}}});
        } else if (Type == "DocumentEditorDiscard" || Type == "DocumentEditorResume") {
            emit EditorReply(binding_, {{"requestType", Type}, {"ok", true}});
            emit EditorReply(binding_, {{"editor", QJsonObject{{"retiring", false}, {"resuming", false}}}});
        }
        return true;
    }
    void Load(quint64 Binding, DocumentOperationTicket Ticket, SourceIdentity) override
    { binding_ = Binding; ticket_ = std::move(Ticket); ++loads_; }
    void Cancel(quint64, DocumentOperationTicket) override { ++cancels_; }
    void Stop(quint64 Binding) override { binding_ = Binding; ++stops_; }
    void Finish() { emit Loaded(binding_, ticket_, ExternalModelRevision{1, 0, "base"}); }
    void AckCancel() { emit Cancelled(binding_, ticket_); }
    void AckStop() { ticket_.lease_.reset(); emit Stopped(binding_); }
};
class Lease final : public hc::storage::IMaterializedFileLease
{
public:
    std::string LocalPath() const override { return "fixture.hwp"; }
    hc::storage::EntrySnapshot Describe() const override { return {}; }
    std::size_t Depth() const override { return 0; }
};
class HwpSdiTests final : public QObject
{
    Q_OBJECT
private slots:
    void hostSessionRetiresBeforeReuse()
    {
        QTemporaryDir Dir; const auto Path = Dir.filePath("source.hwp");
        { QFile File(Path); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("source bytes"); }
        WebProcessRuntime Runtime; Runtime.approvedInventorySha256_ = QByteArray(64, 'a');
        QPointer<FakeBackend> Port;
        HwpHostSession Host(Dir.filePath("cache"), Runtime, [&]() -> std::unique_ptr<HwpSdiBackend> {
            auto Backend = std::make_unique<FakeBackend>(); Port = Backend.get(); return Backend;
        });
        const QJsonObject Open{{"action", "rhwpOpen"}, {"version", 1}, {"path", Path}};
        QCOMPARE(Host.Handle({{"action", "rhwpHostState"}, {"version", 1}})["state"].toString(), QString("idle"));
        const auto Started = Host.Handle(Open); const auto Id = Started["documentId"].toString();
        QVERIFY(!Id.isEmpty()); QCOMPARE(Started["state"].toString(), QString("opening"));
        QCOMPARE(Host.Handle({{"action", "rhwpHostState"}, {"version", 1}})["documentId"].toString(), Id);
        QCOMPARE(Host.Handle({{"action", "rhwpHostState"}, {"version", 1}, {"extra", true}})["error"].toString(), QString("rhwp-host-schema"));
        const auto State = [&](QString IdValue) { return Host.Handle({{"action", "rhwpState"}, {"version", 1}, {"documentId", IdValue}}); };
        QTRY_VERIFY(Port); QTRY_COMPARE(Port->loads_, 1);
        const auto Copy = QString::fromStdString(Port->ticket_.lease_->LocalPath());
        QVERIFY(QFile::exists(Copy)); QVERIFY(Copy != Path);
        QCOMPARE(Host.Handle(Open)["error"].toString(), QString("rhwp-host-busy"));
        QCOMPARE(Host.Handle({{"action", "rhwpClose"}, {"version", 1}, {"documentId", "stale"}})["error"].toString(), QString("rhwp-host-stale-document"));
        QCOMPARE(Port->stops_, 0); Port->Finish();
        QTRY_COMPARE(State(Id)["state"].toString(), QString("ready"));
        QVERIFY(State(Id)["capabilities"].toObject()["view"].toBool());
        QVERIFY(!State(Id)["capabilities"].toObject()["edit"].toBool());
        QCOMPARE(Host.Handle({{"action", "rhwpSave"}, {"version", 1}, {"documentId", Id}})["error"].toString(), QString("rhwp-host-command-unavailable"));
        QCOMPARE(Host.Handle({{"action", "rhwpClose"}, {"version", 1}, {"documentId", Id}})["state"].toString(), QString("closing"));
        QVERIFY(!Host.IsIdle()); QVERIFY(QFile::exists(Copy)); QCOMPARE(Port->stops_, 1);
        QCOMPARE(Host.Handle(Open)["error"].toString(), QString("rhwp-host-busy"));
        Port->AckStop(); QTRY_VERIFY(Host.IsIdle()); QVERIFY(!QFile::exists(Copy));
        QCOMPARE(State(Id)["state"].toString(), QString("closed"));
        const auto Next = Host.Handle(Open)["documentId"].toString(); QVERIFY(!Next.isEmpty()); QVERIFY(Next != Id);
        QCOMPARE(State(Id)["error"].toString(), QString("rhwp-host-stale-document"));
        Host.Shutdown(); QTRY_VERIFY(Host.IsIdle());
        QCOMPARE(Host.Handle(Open)["error"].toString(), QString("rhwp-host-stopping"));
        QVERIFY(QFile::exists(Path));
    }
    void sdkReloadWaitsForRetirement()
    {
        QTemporaryDir Dir; const auto A = Dir.filePath("a.hwp"), B = Dir.filePath("b.hwpx"), C = Dir.filePath("c.hwp");
        for (const auto& Path : {A, B, C}) { QFile File(Path); QVERIFY(File.open(QIODevice::WriteOnly)); File.write(Path.toUtf8()); }
        WebProcessRuntime Runtime; Runtime.approvedInventorySha256_ = QByteArray(64, 'a');
        QPointer<FakeBackend> Port; int Starts = 0;
        HwpHostSession Host(Dir.filePath("cache"), Runtime, [&]() -> std::unique_ptr<HwpSdiBackend> {
            ++Starts; auto Backend = std::make_unique<FakeBackend>(); Port = Backend.get(); return Backend;
        });
        QVERIFY(Host.OpenHost(A)["error"].toString().isEmpty()); QTRY_VERIFY(Port); QTRY_COMPARE(Port->loads_, 1); Port->Finish();
        const auto Old = Port;
        QVERIFY(Host.OpenHost(B)["accepted"].toBool()); QVERIFY(Host.OpenHost(C)["accepted"].toBool());
        QCOMPARE(Starts, 1); QVERIFY(!Host.IsIdle()); Old->AckStop();
        QTRY_COMPARE(Starts, 2); QTRY_VERIFY(Port && Port != Old); QTRY_COMPARE(Port->loads_, 1);
        { QFile Copy(QString::fromStdString(Port->ticket_.lease_->LocalPath())); QVERIFY(Copy.open(QIODevice::ReadOnly)); QCOMPARE(Copy.readAll(), C.toUtf8()); }
        // No intermediate B backend may start. Cancel must clear a queued reload.
        QVERIFY(Host.OpenHost(B)["accepted"].toBool()); Host.CancelHost(); Port->AckStop();
        QTRY_VERIFY(Host.IsIdle()); QTest::qWait(30); QCOMPARE(Starts, 2);
        QVERIFY(Host.OpenHost(C)["error"].toString().isEmpty()); Host.Shutdown(); QTRY_VERIFY(Host.IsIdle());
    }
    void explicitOpenSupersedesDeferredSdkSelection()
    {
        QTemporaryDir Dir; const auto A = Dir.filePath("a.hwp"), B = Dir.filePath("b.hwp"), C = Dir.filePath("c.hwp");
        for (const auto& Path : {A, B, C}) { QFile File(Path); QVERIFY(File.open(QIODevice::WriteOnly)); File.write(Path.toUtf8()); }
        WebProcessRuntime Runtime; Runtime.approvedInventorySha256_ = QByteArray(64, 'a');
        QPointer<FakeBackend> Port; int Starts = 0;
        HwpHostSession Host(Dir.filePath("cache"), Runtime, [&]() -> std::unique_ptr<HwpSdiBackend> {
            ++Starts; auto Backend = std::make_unique<FakeBackend>(); Port = Backend.get(); return Backend;
        });
        Host.OpenHost(A); QTRY_VERIFY(Port); QTRY_COMPARE(Port->loads_, 1); Port->Finish();
        HwpOpenWindow* Window = nullptr;
        for (auto* Widget : QApplication::topLevelWidgets()) if (auto* Candidate = qobject_cast<HwpOpenWindow*>(Widget))
            if (!Candidate->IsClosed()) Window = Candidate;
        QVERIFY(Window); QJsonObject Reopened;
        connect(Window, &HwpOpenWindow::Closed, this, [&] {
            Reopened = Host.Handle({{"action", "rhwpOpen"}, {"version", 1}, {"path", C}});
        });
        Host.OpenHost(B); Port->AckStop(); QTRY_COMPARE(Starts, 2); QTRY_COMPARE(Port->loads_, 1);
        QVERIFY(!Reopened["documentId"].toString().isEmpty());
        { QFile Copy(QString::fromStdString(Port->ticket_.lease_->LocalPath())); QVERIFY(Copy.open(QIODevice::ReadOnly)); QCOMPARE(Copy.readAll(), C.toUtf8()); }
        QTest::qWait(30); QCOMPARE(Starts, 2); QCOMPARE(Port->stops_, 0);
        Host.CancelHost(); Port->AckStop(); QTRY_VERIFY(Host.IsIdle());
    }
    void rejectedHostCommandPreservesRhwp_data()
    {
        QTest::addColumn<QString>("Action"); QTest::addColumn<bool>("Pending");
        QTest::newRow("cancel-ready") << QString("cancelHost") << false;
        QTest::newRow("open-ready") << QString("openHost") << false;
        QTest::newRow("cancel-pending") << QString("cancelHost") << true;
        QTest::newRow("open-pending") << QString("openHost") << true;
    }
    void rejectedHostCommandPreservesRhwp()
    {
        QFETCH(QString, Action); QFETCH(bool, Pending);
        QTemporaryDir Dir; const auto A = Dir.filePath("a.hwp"), B = Dir.filePath("b.hwpx");
        for (const auto& Path : {A, B}) { QFile File(Path); QVERIFY(File.open(QIODevice::WriteOnly)); File.write(Path.toUtf8()); }
        WebProcessRuntime Runtime; Runtime.approvedInventorySha256_ = QByteArray(64, 'a');
        QPointer<FakeBackend> Port; int Starts = 0, NativeCalls = 0;
        HwpHostSession Host(Dir.filePath("cache"), Runtime, [&]() -> std::unique_ptr<HwpSdiBackend> {
            ++Starts; auto Backend = std::make_unique<FakeBackend>(); Port = Backend.get(); return Backend;
        });
        const auto Native = [&](const QJsonObject& Request) -> QJsonObject {
            ++NativeCalls;
            if (Request.contains("protocolVersion")) return {{"error", "document-protocol-version"}};
            return {{"accepted", true}};
        };
        const auto Opened = Host.Dispatch({{"action", "openHost"}, {"path", A}}, Native);
        QTRY_VERIFY(Port); QTRY_COMPARE(Port->loads_, 1); Port->Finish(); QCOMPARE(NativeCalls, 0);
        const QJsonObject State{{"action", "rhwpState"}, {"version", 1}, {"documentId", Opened["documentId"]}};
        QTRY_COMPARE(Host.Handle(State)["state"].toString(), QString("ready"));
        if (Pending) {
            Host.Dispatch({{"action", "openHost"}, {"path", B}}, Native);
            QCOMPARE(Host.Handle(State)["state"].toString(), QString("closing"));
        }
        const auto Stops = Port->stops_;
        const auto Rejected = Host.Dispatch({{"action", Action}, {"path", Dir.filePath("next.txt")}, {"protocolVersion", 99}}, Native);
        QCOMPARE(Rejected["error"].toString(), QString("document-protocol-version"));
        QCOMPARE(NativeCalls, 1); QCOMPARE(Port->stops_, Stops); QVERIFY(!Host.IsIdle());
        if (Pending) {
            Port->AckStop(); QTRY_COMPARE(Starts, 2); QTRY_COMPARE(Port->loads_, 1); Port->Finish();
            QTRY_VERIFY(([&] {
                for (auto* Widget : QApplication::topLevelWidgets()) if (auto* Window = qobject_cast<HwpOpenWindow*>(Widget))
                    if (Window->Document() && Window->Document()->Snapshot().source_.localPath_ == Port->ticket_.lease_->LocalPath()
                        && Window->Document()->Snapshot().lifecycle_ == DocumentLifecycle::Ready) return true;
                return false;
            })());
            QFile Copy(QString::fromStdString(Port->ticket_.lease_->LocalPath())); QVERIFY(Copy.open(QIODevice::ReadOnly)); QCOMPARE(Copy.readAll(), B.toUtf8());
        }
        // A successful typed envelope with the same action is not a legacy SDK selection.
        Host.Dispatch({{"action", "cancelHost"}, {"messageType", "fixture"}}, Native);
        QCOMPARE(Port->stops_, 0);
        Host.Dispatch({{"action", Action}, {"path", Dir.filePath("next.txt")}}, Native);
        QCOMPARE(Port->stops_, 1); Port->AckStop(); QTRY_VERIFY(Host.IsIdle());
    }
    void nativeCallbackCanDeleteSession()
    {
        for (const auto Action : {"cancelHost", "openHost"}) {
            auto Owner = std::make_unique<HwpHostSession>(QString{}, WebProcessRuntime{});
            QPointer<HwpHostSession> Guard(Owner.get());
            const auto Reply = Owner->Dispatch({{"action", Action}, {"path", "next.txt"}}, [&](const QJsonObject&) {
                Owner.reset(); return QJsonObject{{"accepted", true}};
            });
            QVERIFY(!Guard); QVERIFY(Reply["accepted"].toBool());
        }
    }
    void hostSessionRetainsTerminalError_data()
    {
        QTest::addColumn<bool>("ExistingSource");
        QTest::newRow("missing-source") << false;
        QTest::newRow("missing-runtime") << true;
    }
    void hostSessionRetainsTerminalError()
    {
        QTemporaryDir Dir; WebProcessRuntime Runtime; Runtime.approvedInventorySha256_ = QByteArray(64, 'a');
        QFETCH(bool, ExistingSource);
        Runtime.root_ = Dir.filePath("missing-runtime");
        if (ExistingSource) { QFile File(Dir.filePath("missing.hwp")); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("source bytes"); }
        HwpHostSession Host(Dir.filePath("cache"), Runtime);
        const auto Opened = Host.Handle({{"action", "rhwpOpen"}, {"version", 1}, {"path", Dir.filePath("missing.hwp")}});
        const auto Id = Opened["documentId"].toString(); QVERIFY(!Id.isEmpty());
        const QJsonObject State{{"action", "rhwpState"}, {"version", 1}, {"documentId", Id}};
        QTRY_COMPARE(Host.Handle(State)["state"].toString(), QString("failed"));
        const auto Error = Host.Handle(State)["error"].toString(); QVERIFY(!Error.isEmpty());
        Host.Handle({{"action", "rhwpClose"}, {"version", 1}, {"documentId", Id}});
        QTRY_VERIFY(Host.IsIdle()); QCOMPARE(Host.Handle(State)["state"].toString(), QString("closed"));
        QCOMPARE(Host.Handle(State)["error"].toString(), Error);
    }
    void hostSessionRejectsBeforeAcquisition()
    {
        QTemporaryDir Dir; WebProcessRuntime Runtime;
        Runtime.approvedInventorySha256_ = QByteArray{};
        HwpHostSession Host(Dir.filePath("cache"), Runtime);
        const auto Request = QJsonObject{{"action", "rhwpOpen"}, {"version", 1}, {"path", Dir.filePath("source.hwp")}};
        QCOMPARE(Host.Handle(Request)["error"].toString(), QString("rhwp-runtime-policy-unavailable"));
        auto Bad = Request; Bad["version"] = "1";
        QCOMPARE(Host.Handle(Bad)["error"].toString(), QString("rhwp-host-schema"));
        Bad = Request; Bad["write"] = true;
        QCOMPARE(Host.Handle(Bad)["error"].toString(), QString("rhwp-host-schema"));
        Bad = Request; Bad["path"] = "relative.hwp";
        QCOMPARE(Host.Handle(Bad)["error"].toString(), QString("rhwp-host-path"));
        QVERIFY(Host.IsIdle()); QVERIFY(!QFile::exists(Dir.filePath("cache")));
    }
    void sharedProductRuntime_data()
    {
        QTest::addColumn<QString>("ApplicationDirectory"); QTest::addColumn<QString>("Expected");
        // The runtime shares the product ROOT; no parent directory is searched.
        QTest::newRow("product") << "D:/Product" << "D:/Product";
        QTest::newRow("native-separators") << QStringLiteral(R"(D:\Product\)") << "D:/Product";
        QTest::newRow("no-parent-search") << "D:/Product/Addons/WLX/HyperViewer" << "D:/Product/Addons/WLX/HyperViewer";
        QTest::newRow("relative-rejected") << "Addons/WLX/HyperViewer" << "";
        QTest::newRow("empty-rejected") << "" << "";
    }
    void sharedProductRuntime()
    {
        QFETCH(QString, ApplicationDirectory); QFETCH(QString, Expected);
        const QByteArray Pin(64, 'a');
        const auto Runtime = MakeProductHwpRuntime(ApplicationDirectory, Pin);
        QCOMPARE(Runtime.root_, Expected);
        QVERIFY(Runtime.sha256_.isEmpty()); QVERIFY(Runtime.approvedInventorySha256_.has_value());
        QCOMPARE(*Runtime.approvedInventorySha256_, Pin);
        const auto Unapproved = MakeProductHwpRuntime(ApplicationDirectory, {});
        QVERIFY(Unapproved.approvedInventorySha256_.has_value());
        QVERIFY(Unapproved.approvedInventorySha256_->isEmpty());
    }
    void openWindowPolicyRejectsBeforeAcquisition()
    {
        QTemporaryDir Dir; WebProcessRuntime Runtime;
        Runtime.approvedInventorySha256_ = QByteArray{};
        int Factories = 0;
        HwpOpenWindow Window(Dir.filePath("missing.hwp"), Dir.filePath("cache"), Runtime,
            [&]() -> std::unique_ptr<HwpSdiBackend> { ++Factories; return {}; });
        QVERIFY(Window.windowTitle().contains(QStringLiteral("불러오는 중")));
        QCOMPARE(Window.findChild<QLabel*>()->text(), QStringLiteral("문서를 준비하고 있습니다…"));
        Window.show(); Window.Open();
        QCOMPARE(Window.findChild<QLabel*>()->text(), QStringLiteral("현재 설치에서는 한글 문서 보기를 사용할 수 없습니다."));
        QCOMPARE(Window.Error(), QString("rhwp-runtime-policy-unavailable"));
        QVERIFY(!Window.IsPreparing()); QVERIFY(!QFile::exists(Dir.filePath("cache"))); QCOMPARE(Factories, 0);
        Window.close(); QTRY_VERIFY(Window.IsClosed()); QVERIFY(!Window.isVisible());
    }
    void asynchronousOpenOwner_data()
    {
        QTest::addColumn<QString>("Mode");
        for (const auto* Mode : {"ready-close", "ready-quit", "cancel-preparation", "missing-source", "no-backend"})
            QTest::newRow(Mode) << QString::fromLatin1(Mode);
    }
    void asynchronousOpenOwner()
    {
        QFETCH(QString, Mode); QTemporaryDir Dir;
        const auto Original = Dir.filePath("source.hwp");
        if (Mode != "missing-source") {
            QFile File(Original); QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write("source bytes"), qint64{12});
        }
        WebProcessRuntime Runtime; Runtime.approvedInventorySha256_ = QByteArray(64, 'a');
        FakeBackend* Port = nullptr; int Factories = 0, Ticks = 0; QString Copy;
        HwpOpenWindow Window(Original, Dir.filePath("cache"), Runtime, [&]() -> std::unique_ptr<HwpSdiBackend> {
            ++Factories; if (Mode == "no-backend") return {};
            auto Backend = std::make_unique<FakeBackend>(); Port = Backend.get(); return Backend;
        });
        QSignalSpy Closed(&Window, &HwpOpenWindow::Closed);
        QTimer Timer; Timer.setInterval(1); connect(&Timer, &QTimer::timeout, this, [&] { ++Ticks; }); Timer.start();
        Window.show(); Window.Open(); QVERIFY(Window.IsPreparing()); Window.Open();
        if (Mode == "cancel-preparation") {
            Window.RequestClose(); QVERIFY(!Window.IsClosed());
        } else if (Mode == "missing-source" || Mode == "no-backend") {
            QTRY_VERIFY(!Window.Error().isEmpty()); Window.RequestClose();
        } else {
            QTRY_VERIFY(Window.Document()); QVERIFY(Port); QCOMPARE(Factories, 1);
            Copy = QString::fromStdString(Window.Document()->Snapshot().source_.localPath_);
            QVERIFY(Copy != Original); QVERIFY(QFile::exists(Copy));
            QVERIFY(!Window.Document()->Snapshot().source_.originalWritable_);
            Port->Finish(); QTRY_VERIFY(Window.Document()->Capabilities().view_);
            if (Mode == "ready-quit") { QEvent Quit(QEvent::Quit); QCoreApplication::sendEvent(qApp, &Quit); }
            else Window.close();
            QVERIFY(!Window.IsClosed()); QTRY_COMPARE(Port->stops_, 1); QVERIFY(QFile::exists(Copy));
            Port->AckStop();
        }
        QTRY_VERIFY_WITH_TIMEOUT(Window.IsClosed(), 10000); QCOMPARE(Closed.count(), 1);
        QVERIFY(!Window.Document()); QVERIFY(Ticks > 0);
        if (!Copy.isEmpty()) QVERIFY(!QFile::exists(Copy));
        if (Mode == "cancel-preparation" || Mode == "missing-source") QCOMPARE(Factories, 0);
        // Cached content is removed only after the SDI and its backend retire.
        QDirIterator Files(Dir.filePath("cache"), QDir::Files, QDirIterator::Subdirectories);
        while (Files.hasNext()) QVERIFY2(!Files.next().endsWith(".hwp"), "materialized document leaked");
        if (Mode != "missing-source") { QFile File(Original); QVERIFY(File.open(QIODevice::ReadOnly)); QCOMPARE(File.readAll(), QByteArray("source bytes")); }
    }
    void observerDeletesOpenOwner_data()
    {
        QTest::addColumn<QString>("Mode");
        for (const auto* Mode : {"missing", "no-backend", "opening", "factory"}) QTest::newRow(Mode) << QString::fromLatin1(Mode);
    }
    void observerDeletesOpenOwner()
    {
        QFETCH(QString, Mode); QTemporaryDir Dir; const auto Path = Dir.filePath("source.hwp");
        if (Mode != "missing") { QFile File(Path); QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write("source bytes"), qint64{12}); }
        WebProcessRuntime Runtime; Runtime.approvedInventorySha256_ = QByteArray(64, 'a');
        std::unique_ptr<HwpOpenWindow> Window; QString Copy;
        const auto CaptureCopy = [&] {
            QDirIterator Files(Dir.filePath("cache"), QDir::Files, QDirIterator::Subdirectories);
            while (Files.hasNext()) { const auto File = Files.next(); if (File.endsWith(".hwp")) Copy = File; }
        };
        Window = std::make_unique<HwpOpenWindow>(Path, Dir.filePath("cache"), Runtime,
            [&]() -> std::unique_ptr<HwpSdiBackend> {
                if (Mode == "factory") { CaptureCopy(); Window.reset(); return {}; }
                if (Mode == "no-backend") return {};
                return std::make_unique<FakeBackend>();
            });
        connect(Window.get(), &HwpOpenWindow::StateChanged, this, [&] {
            if (!Window) return;
            if ((Mode == "opening" && Window->Document())
                || ((Mode == "missing" || Mode == "no-backend") && !Window->Error().isEmpty())) {
                CaptureCopy(); Window.reset();
            }
        });
        Window->show(); Window->Open(); QTRY_VERIFY_WITH_TIMEOUT(!Window, 10000);
        if (Mode != "missing") { QVERIFY(!Copy.isEmpty()); QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(Copy), 10000); }
    }
    void applicationQuitDrainsOwners_data()
    {
        QTest::addColumn<int>("Count"); QTest::newRow("one-no-auto-quit") << 1; QTest::newRow("two-owners") << 2;
    }
    void applicationQuitDrainsOwners()
    {
        QFETCH(int, Count); QTemporaryDir Dir; const auto Path = Dir.filePath("source.hwp");
        { QFile File(Path); QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write("source bytes"), qint64{12}); }
        const auto AutoQuit = QApplication::quitOnLastWindowClosed(); QApplication::setQuitOnLastWindowClosed(false);
        const auto Restore = qScopeGuard([&] { QApplication::setQuitOnLastWindowClosed(AutoQuit); });
        WebProcessRuntime Runtime; Runtime.approvedInventorySha256_ = QByteArray(64, 'a');
        std::vector<std::unique_ptr<HwpOpenWindow>> Windows; std::vector<QPointer<FakeBackend>> Ports; QStringList Copies;
        for (int Index = 0; Index < Count; ++Index) {
            auto Window = std::make_unique<HwpOpenWindow>(Path, Dir.filePath("cache"), Runtime, [&] {
                auto Backend = std::make_unique<FakeBackend>(); Ports.push_back(Backend.get()); return Backend;
            });
            Window->show(); Window->Open(); Windows.push_back(std::move(Window));
        }
        for (const auto& Window : Windows) {
            QTRY_VERIFY(Window->Document()); Copies.push_back(QString::fromStdString(Window->Document()->Snapshot().source_.localPath_));
        }
        QTimer Acknowledge; Acknowledge.setInterval(5);
        connect(&Acknowledge, &QTimer::timeout, this, [&] {
            for (const auto& Port : Ports) if (Port && Port->stops_) { Port->stops_ = 0; Port->AckStop(); }
        }); Acknowledge.start();
        QTimer Timeout; Timeout.setSingleShot(true); Timeout.start(10000);
        connect(&Timeout, &QTimer::timeout, qApp, [] { QCoreApplication::exit(99); });
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
        const auto Result = qApp->exec(); Timeout.stop(); QCOMPARE(Result, 0);
        for (const auto& Window : Windows) QVERIFY(Window->IsClosed());
        for (const auto& Copy : Copies) QVERIFY(!QFile::exists(Copy));
    }
    void destroyedOpenOwnerReleasesCopy()
    {
        QTemporaryDir Dir; const auto Path = Dir.filePath("source.hwp");
        { QFile File(Path); QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write("source bytes"), qint64{12}); }
        WebProcessRuntime Runtime; Runtime.approvedInventorySha256_ = QByteArray(64, 'a');
        auto Window = std::make_unique<HwpOpenWindow>(Path, Dir.filePath("cache"), Runtime,
            [] { return std::make_unique<FakeBackend>(); });
        Window->show(); Window->Open(); QTRY_VERIFY(Window->Document());
        const auto Copy = QString::fromStdString(Window->Document()->Snapshot().source_.localPath_);
        QVERIFY(QFile::exists(Copy)); Window.reset();
        QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(Copy), 10000);
    }
    void runtimeDeniedAfterPreparation()
    {
        QTemporaryDir Dir; const auto Path = Dir.filePath("source.hwp");
        { QFile File(Path); QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write("source bytes"), qint64{12}); }
        WebProcessRuntime Runtime; Runtime.root_ = Dir.filePath("missing-runtime");
        Runtime.approvedInventorySha256_ = QByteArray(64, 'a');
        HwpOpenWindow Window(Path, Dir.filePath("cache"), Runtime);
        Window.show(); Window.Open(); QTRY_VERIFY(Window.Document());
        QTRY_COMPARE_WITH_TIMEOUT(Window.Document()->Snapshot().lifecycle_, DocumentLifecycle::Failed, 10000);
        QCOMPARE(Window.Document()->Snapshot().error_->code_, std::string("rhwp-runtime-inventory-missing"));
        const auto Copy = QString::fromStdString(Window.Document()->Snapshot().source_.localPath_);
        QVERIFY(QFile::exists(Copy)); Window.RequestClose(); QTRY_VERIFY_WITH_TIMEOUT(Window.IsClosed(), 10000);
        QVERIFY(!QFile::exists(Copy)); QVERIFY(HwpProcessBackend::IsDrained());
    }
    void independentReadOnlyWindows()
    {
        auto A = std::make_unique<FakeBackend>(); auto* Port = A.get();
        HwpSdiWindow Window("a", {}, std::move(A));
        HwpSdiWindow Other("b", {}, std::make_unique<FakeBackend>());
        QVERIFY(Window.isWindow()); QVERIFY(Other.isWindow());
        QVERIFY(!Window.parentWidget()); Window.show(); QVERIFY(Window.Open());
        QVERIFY(Port->isVisible()); QVERIFY(!Port->isEnabled());
        QVERIFY(Port->width() > 0); QVERIFY(Port->height() > 0);
        QVERIFY(Window.IsBusy()); QVERIFY(!Window.Open());
        Port->Finish();
        QVERIFY(Window.IsBusy()); // GUI callback is always queued.
        QTRY_VERIFY(!Window.IsBusy());
        QVERIFY(Window.Capabilities().view_); QVERIFY(!Window.Capabilities().edit_);
        QVERIFY(Port->isVisible()); QVERIFY(Port->isEnabled());
        QVERIFY(!Window.Capabilities().save_); QVERIFY(!Window.Capabilities().saveAs_);
        QVERIFY(!Other.Capabilities().view_);
        QVERIFY(Window.findChildren<QMenuBar*>().isEmpty());
        QVERIFY(Window.findChildren<QToolBar*>().isEmpty());
        QVERIFY(Window.findChildren<QTabWidget*>().isEmpty());
        QVERIFY(Window.findChildren<QPlainTextEdit*>().isEmpty());
        Window.RequestClose(); Port->AckStop();
        QTRY_COMPARE(Window.Snapshot().lifecycle_, DocumentLifecycle::Closed);
    }
    void sourceAuthorityDoesNotReload()
    {
        auto A = std::make_unique<FakeBackend>(); auto* Port = A.get();
        SourceIdentity Source; Source.localPath_ = "temporary/input.hwp";
        Source.inputSha256_ = std::string(64, 'a');
        HwpSdiWindow Window("a", std::move(Source), std::move(A));
        QVERIFY(Window.Open()); Port->Finish(); QTRY_VERIFY(!Window.IsBusy());
        const auto Before = Window.Snapshot();
        SourceAuthorityBinding Binding{Before.id_, Before.generation_, Before.revision_, Before.model_,
            Before.source_.inputSha256_, std::string(32, 'b')};
        Binding.origin_.id_ = {{"provider"}, "entry", hc::storage::IdentityScope::Persistent,
            hc::storage::ResourceRevision{"r1"}};
        Binding.originalWritable_ = true;
        QVERIFY(Window.BindSourceAuthority(Binding).HasValue());
        QCOMPARE(Port->loads_, 1);
        QCOMPARE(Window.Snapshot().generation_, Before.generation_);
        QCOMPARE(Window.Snapshot().model_, Before.model_);
        QCOMPARE(Window.Snapshot().source_.localPath_, Before.source_.localPath_);
        QVERIFY(!Window.Capabilities().save_);
        QVERIFY(!Window.BindSourceAuthority(Binding).HasValue());
        Window.RequestClose();
        QVERIFY(!Window.BindSourceAuthority(Binding).HasValue());
        Port->AckStop(); QTRY_VERIFY(!Window.IsBusy());
    }
    void printCloseWaitsForExactRetirement() {
        auto Backend=std::make_unique<FakeBackend>();auto* Port=Backend.get();Port->editorEnabled_=true;
        HwpSdiWindow Window("print-close",{},std::move(Backend),true);
        QVERIFY(Window.Open());Port->Finish();QTRY_VERIFY(!Window.IsBusy());
        const auto Before=Window.Snapshot();QVERIFY(Window.Print());QVERIFY(!Window.Print());
        const auto Identity=Port->command_["printIdentity"].toObject();
        Window.RequestClose();QVERIFY(Window.IsBusy());QCOMPARE(Port->stops_,0);
        emit Port->EditorReply(Port->binding_,{{"printIdle",true}});
        QTest::qWait(20);QVERIFY(Window.IsBusy());QCOMPARE(Port->stops_,0);
        auto Stale=Identity;Stale["token"]="99999";
        emit Port->EditorReply(Port->binding_,{{"printIdle",true},{"printRetired",Stale}});
        QTest::qWait(20);QVERIFY(Window.IsBusy());QCOMPARE(Port->stops_,0);
        emit Port->EditorReply(Port->binding_,{{"printIdle",true},{"printRetired",Identity}});
        QTRY_VERIFY(!Window.IsBusy());QCOMPARE(Port->stops_,0);
        QCOMPARE(Window.Snapshot().dirty_,Before.dirty_);QVERIFY(Window.Snapshot().savedModel_==Before.savedModel_);
        Window.RequestClose();QCOMPARE(Port->stops_,1);Port->AckStop();
        QTRY_COMPARE(Window.Snapshot().lifecycle_,DocumentLifecycle::Closed);
    }
    void invalidPrintRetiresBeforeReuse() {
        auto Backend=std::make_unique<FakeBackend>();auto* Port=Backend.get();Port->editorEnabled_=true;
        HwpSdiWindow Window("print-error",{},std::move(Backend),true);
        QVERIFY(Window.Open());Port->Finish();QTRY_VERIFY(!Window.IsBusy());
        QVERIFY(Window.Print());const auto Identity=Port->command_["printIdentity"].toObject();
        QSignalSpy Errors(&Window,&HwpSdiWindow::PrintFailed);
        emit Port->EditorReply(Port->binding_,{{"printMessage",QJsonObject{{"type","PrintCommit"}}}});
        QTRY_COMPARE(Errors.count(),1);QVERIFY(Window.IsBusy());QVERIFY(!Window.Print());
        emit Port->EditorReply(Port->binding_,{{"printIdle",true},{"printRetired",Identity}});
        QTRY_VERIFY(!Window.IsBusy());QVERIFY(Window.Print());
        Window.RequestClose(CloseDisposition::Discard);QCOMPARE(Port->stops_,1);Port->AckStop();
        QTRY_COMPARE(Window.Snapshot().lifecycle_,DocumentLifecycle::Closed);
    }
    void nativePrintCancelAndDuplicate() {
        const auto Corpus=qEnvironmentVariable("HC_RHWP_TEST_CORPUS");if(Corpus.isEmpty())QSKIP("Corpus required");
        QTemporaryDir Dir;WebProcessRuntime Runtime;Runtime.approvedInventorySha256_=QByteArray(64,'a');
        QPointer<FakeBackend> Port;int Dialogs=0;HwpOpenWindow* Owner=nullptr;
        HwpOpenWindow Window(Corpus+"/c02.hwp",Dir.filePath("cache"),Runtime,[&] {
            auto P=std::make_unique<FakeBackend>();Port=P.get();return P;
        },false,true,[&](QPrinter&,QWidget*) {++Dialogs;Owner->Print();return false;});
        Owner=&Window;Window.show();Window.Open();QTRY_VERIFY(Port);Port->Finish();QTRY_VERIFY(!Window.Document()->IsBusy());
        const auto Before=Window.Document()->Snapshot();Window.Print();QCOMPARE(Dialogs,1);
        QCOMPARE(Window.Document()->Snapshot().dirty_,Before.dirty_);QVERIFY(Window.Document()->Snapshot().savedModel_==Before.savedModel_);
        Window.RequestClose();QCOMPARE(Port->stops_,1);Port->AckStop();QTRY_VERIFY(Window.IsClosed());
    }
    void actualPrintCloseRetiresHelper_data() {
        QTest::addColumn<bool>("Editable");QTest::newRow("view")<<false;QTest::newRow("edit")<<true;
    }
    void actualPrintCloseRetiresHelper() {
        QFETCH(bool,Editable);
        const auto RuntimeRoot=qEnvironmentVariable("HC_RHWP_TEST_PRODUCT_RUNTIME"),Corpus=qEnvironmentVariable("HC_RHWP_TEST_CORPUS");
        if(RuntimeRoot.isEmpty()||Corpus.isEmpty())QSKIP("Qualified runtime and corpus required");
        QFile Inventory(RuntimeRoot+"/runtime-inventory.json");QVERIFY(Inventory.open(QIODevice::ReadOnly));WebProcessRuntime Runtime;
        Runtime.root_=RuntimeRoot;Runtime.approvedInventorySha256_=QCryptographicHash::hash(Inventory.readAll(),QCryptographicHash::Sha256).toHex();
        QTemporaryDir Dir;const auto Cache=Dir.filePath("cache");
        HwpOpenWindow Window(Corpus+"/c07.hwpx",Cache,Runtime,{},Editable,true,[&](QPrinter& Printer,QWidget*) {
            Printer.setOutputFormat(QPrinter::PdfFormat);Printer.setOutputFileName(Dir.filePath("cancelled.pdf"));return true;
        });
        Window.show();Window.Open();QTRY_VERIFY_WITH_TIMEOUT(Window.Document(),15000);
        QTRY_VERIFY_WITH_TIMEOUT(!Window.Document()->IsBusy(),45000);
        QCOMPARE(Window.Document()->Snapshot().lifecycle_,DocumentLifecycle::Ready);
        QSignalSpy Errors(Window.Document(),&HwpSdiWindow::PrintFailed);
        Window.Print();QVERIFY(Window.Document()->IsPrinting());Window.RequestClose();
        QTRY_VERIFY_WITH_TIMEOUT(Window.IsClosed(),30000);QCOMPARE(Errors.count(),0);
        QDirIterator Files(Cache,QDir::Files,QDirIterator::Subdirectories);QVERIFY(!Files.hasNext());
        QVERIFY(!QFile::exists(Dir.filePath("cancelled.pdf")));
    }
    void actualNativePrint_data() {
        QTest::addColumn<QString>("Fixture");QTest::addColumn<bool>("Editable");QTest::addColumn<bool>("Range");
        QTest::newRow("hwp-view")<<"c02.hwp"<<false<<false;
        QTest::newRow("hwp-edit")<<"c02.hwp"<<true<<false;
        QTest::newRow("hwpx-multipage-view")<<"c07.hwpx"<<false<<false;
        QTest::newRow("hwpx-edit")<<"c05.hwpx"<<true<<false;
        QTest::newRow("hwpx-range-landscape")<<"c07.hwpx"<<false<<true;
    }
    void actualNativePrint() {
        QFETCH(QString,Fixture);QFETCH(bool,Editable);QFETCH(bool,Range);
        const auto RuntimeRoot=qEnvironmentVariable("HC_RHWP_TEST_PRODUCT_RUNTIME"),Corpus=qEnvironmentVariable("HC_RHWP_TEST_CORPUS");
        const auto Evidence=qEnvironmentVariable("HC_RHWP_TEST_PRINT_EVIDENCE");
        if(RuntimeRoot.isEmpty()||Corpus.isEmpty()||Evidence.isEmpty())QSKIP("Qualified runtime, corpus and evidence required");
        QFile Inventory(RuntimeRoot+"/runtime-inventory.json");QVERIFY(Inventory.open(QIODevice::ReadOnly));WebProcessRuntime Runtime;
        Runtime.root_=RuntimeRoot;Runtime.approvedInventorySha256_=QCryptographicHash::hash(Inventory.readAll(),QCryptographicHash::Sha256).toHex();
        QTemporaryDir Dir;const auto Input=Corpus+'/'+Fixture;QFile Source(Input);QVERIFY(Source.open(QIODevice::ReadOnly));const auto BeforeBytes=Source.readAll();Source.close();
        const auto Output=Evidence+'/'+QString::fromLatin1(QTest::currentDataTag())+".pdf";QFile::remove(Output);
        HwpOpenWindow Window(Input,Dir.filePath("cache"),Runtime,{},Editable,true,[&](QPrinter& Printer,QWidget*) {
            Printer.setOutputFormat(QPrinter::PdfFormat);Printer.setOutputFileName(Output);
            Printer.setPageSize(QPageSize(Range?QPageSize::A5:QPageSize::A4));
            Printer.setPageOrientation(Range?QPageLayout::Landscape:QPageLayout::Portrait);
            if(Range){Printer.setPrintRange(QPrinter::PageRange);Printer.setFromTo(2,3);}return true;
        });
        Window.show();Window.Open();QTRY_VERIFY_WITH_TIMEOUT(Window.Document(),15000);QTRY_VERIFY_WITH_TIMEOUT(!Window.Document()->IsBusy(),45000);
        QCOMPARE(Window.Document()->Snapshot().lifecycle_,DocumentLifecycle::Ready);
        QSignalSpy PrintErrors(Window.Document(),&HwpSdiWindow::PrintFailed);
        const auto Before=Window.Document()->Snapshot();Window.Print();QVERIFY(Window.Document()->IsPrinting());Window.Print();
        QTRY_VERIFY_WITH_TIMEOUT(!Window.Document()->IsBusy(),60000);
        const auto After=Window.Document()->Snapshot();
        if(!PrintErrors.isEmpty())qWarning()<<PrintErrors;
        QVERIFY2(!After.error_,After.error_?After.error_->code_.c_str():"");QCOMPARE(After.dirty_,Before.dirty_);
        QCOMPARE(After.savedRevision_,Before.savedRevision_);QVERIFY(After.savedModel_==Before.savedModel_);
        QFile Pdf(Output);QVERIFY(Pdf.open(QIODevice::ReadOnly));QVERIFY(Pdf.read(5)=="%PDF-");Pdf.close();
        QVERIFY(Source.open(QIODevice::ReadOnly));QCOMPARE(Source.readAll(),BeforeBytes);Source.close();
        Window.grab().save(Evidence+'/'+QString::fromLatin1(QTest::currentDataTag())+".png");
        Window.RequestClose();QTRY_VERIFY_WITH_TIMEOUT(Window.IsClosed(),15000);
    }
    void actualProductLoad_data()
    {
        QTest::addColumn<bool>("Editable");
        QTest::newRow("view") << false; QTest::newRow("edit") << true;
    }
    void actualProductLoad()
    {
        QFETCH(bool, Editable);
        const auto Root = qEnvironmentVariable("HC_RHWP_TEST_PRODUCT_RUNTIME");
        if (Root.isEmpty()) QSKIP("Approved product runtime not requested");
        QFile Inventory(Root + "/runtime-inventory.json"); QVERIFY(Inventory.open(QIODevice::ReadOnly));
        WebProcessRuntime Runtime; Runtime.root_ = Root;
        Runtime.approvedInventorySha256_ = QCryptographicHash::hash(Inventory.readAll(), QCryptographicHash::Sha256).toHex();
        QTemporaryDir Dir;
        HwpOpenWindow Window(qEnvironmentVariable("HC_RHWP_TEST_CORPUS") + "/c02.hwp", Dir.filePath("cache"), Runtime, {}, Editable);
        Window.show(); Window.Open();
        QTRY_VERIFY_WITH_TIMEOUT(Window.Document() || !Window.Error().isEmpty(), 15000);
        QVERIFY2(Window.Error().isEmpty(), qPrintable(Window.Error()));
        QTRY_VERIFY_WITH_TIMEOUT(!Window.Document()->IsBusy(), 45000);
        const auto State = Window.Document()->Snapshot();
        const auto Code = State.error_ ? State.error_->code_ : std::string{};
        Window.RequestClose(); QTRY_VERIFY_WITH_TIMEOUT(Window.IsClosed(), 15000);
        QVERIFY2(Code.empty(), Code.c_str());
        QCOMPARE(State.lifecycle_, DocumentLifecycle::Ready);
    }
    void nativeOriginalSaveAndConflict()
    {
        const auto Corpus = qEnvironmentVariable("HC_RHWP_TEST_CORPUS");
        if (Corpus.isEmpty()) QSKIP("Pinned native save corpus not requested");
        QFile Fixture(Corpus + "/c02.hwp"); QVERIFY(Fixture.open(QIODevice::ReadOnly)); const auto Bytes = Fixture.readAll();
        QCOMPARE(QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256).toHex(),
            QByteArray("86e607233a2218928fabe5969155116a46d4ab4400db1402fe45b0082149496c"));
        QTemporaryDir Dir; auto Path = Dir.filePath("original.hwp");
        { QFile File(Path); QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write(Bytes), Bytes.size()); }
        WebProcessRuntime Runtime; Runtime.approvedInventorySha256_ = QByteArray(64, 'a');
        QPointer<FakeBackend> Port;
        HwpOpenWindow Window(Path, Dir.filePath("cache"), Runtime, [&]() -> std::unique_ptr<HwpSdiBackend> {
            auto Backend = std::make_unique<FakeBackend>(); Backend->editorEnabled_ = true; Port = Backend.get(); return Backend;
        }, true);
        Window.show(); Window.Open(); QTRY_VERIFY(Port); QTRY_COMPARE(Port->loads_, 1); Port->Finish();
        QTRY_VERIFY(Window.Document() && !Window.Document()->IsBusy());
        const auto Feed = [&] {
            const auto Identity = Port->command_["exportIdentity"].toObject();
            const auto Send = [&](QJsonObject Message) {
                Message["version"] = 1; Message["identity"] = Identity;
                emit Port->EditorReply(Port->binding_, QJsonObject{{"message", Message}});
            };
            Send({{"type", "ExportBegin"}, {"format", "hwp"}, {"byteLength", Bytes.size()},
                {"sha256", QString::fromLatin1(QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256).toHex())},
                {"loss", QJsonObject{{"schemaVersion",1},{"outputFormat","hwp"},{"count",0},{"losses",QJsonArray{}}}}});
            for (qsizetype Offset = 0; Offset < Bytes.size(); Offset += 192 * 1024)
                Send({{"type","ExportChunk"},{"offset",Offset},{"data",QString::fromLatin1(Bytes.mid(Offset,192*1024).toBase64())}});
            Send({{"type","ExportCommit"}});
        };
        emit Port->ModelChanged(Port->binding_, ExternalModelRevision{1, 1, "change-1"});
        QTRY_VERIFY(Window.Document()->Snapshot().dirty_); Window.Save();
        QCOMPARE(Port->command_["type"].toString(), QString("DocumentEditorExport")); Feed();
        QTRY_VERIFY_WITH_TIMEOUT(!Window.Document()->IsBusy(), 10000);
        QVERIFY(!Window.Document()->Snapshot().dirty_);
        QCOMPARE(Port->command_["type"].toString(), QString("DocumentEditorSaved"));
        const auto NativeDialogsDisabled = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
        const auto RestoreDialogs = qScopeGuard([&] { QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, NativeDialogsDisabled); });
        for (const auto& Name : {QString("new.hwp"), QString("existing.hwp")}) {
            const auto Target = Dir.filePath(Name);
            if (Name == "existing.hwp") { QFile File(Target); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("old destination"); }
            bool Chosen = false; QTimer Choose; Choose.setInterval(20);
            connect(&Choose, &QTimer::timeout, this, [&] {
                if (auto* Dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget())) {
                    if (!Chosen) {
                        Chosen = true;
                        // Menu/shortcut actions can arrive while the native dialog
                        // pumps events. They must neither nest a dialog nor export.
                        Window.Save(true); Window.Save();
                        Dialog->selectFile(Target); QMetaObject::invokeMethod(Dialog, "accept", Qt::QueuedConnection);
                    }
                } else if (auto* Box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                    if (auto* Yes = Box->button(QMessageBox::Yes)) Yes->click();
                }
            }); Choose.start(); Window.Save(true); Choose.stop(); QVERIFY(Chosen);
            QTRY_COMPARE(Port->command_["type"].toString(), QString("DocumentEditorExport")); Feed();
            QTRY_VERIFY_WITH_TIMEOUT(!Window.Document()->IsBusy() && Port->command_["type"] == "DocumentEditorSaved", 10000);
            { QFile File(Target); QVERIFY(File.open(QIODevice::ReadOnly)); QCOMPARE(File.readAll(), Bytes); }
            { QFile File(Path); QVERIFY(File.open(QIODevice::ReadOnly)); QCOMPARE(File.readAll(), Bytes); }
            Path = Target;
        }
        emit Port->ModelChanged(Port->binding_, ExternalModelRevision{1, 2, "change-2"});
        QTRY_VERIFY(Window.Document()->Snapshot().dirty_);
        const auto Changed = Bytes + QByteArray("external change");
        { QFile File(Path); QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write(Changed), Changed.size()); }
        int Warnings = 0; QTimer Dialogs; Dialogs.setInterval(20);
        connect(&Dialogs, &QTimer::timeout, this, [&] {
            if (auto* Box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) { ++Warnings; Box->accept(); }
        }); Dialogs.start();
        Window.Save(); QCOMPARE(Port->command_["type"].toString(), QString("DocumentEditorExport")); Feed();
        QTRY_VERIFY_WITH_TIMEOUT(Warnings > 0 && !Window.Document()->IsBusy(), 10000); Dialogs.stop();
        QVERIFY(Window.Document()->Snapshot().dirty_);
        { QFile File(Path); QVERIFY(File.open(QIODevice::ReadOnly)); QCOMPARE(File.readAll(), Changed); }
        Window.Document()->RequestClose(CloseDisposition::Discard); Port->AckStop();
        QTRY_VERIFY_WITH_TIMEOUT(Window.IsClosed(), 10000);
    }
    void actualRecoveredProductLoad_data() { checkpointAndReopen_data(); }
    void actualRecoveredProductLoad() {
        QFETCH(QString, Name); QFETCH(QString, Format);
        const auto RuntimeRoot=qEnvironmentVariable("HC_RHWP_TEST_PRODUCT_RUNTIME");
        const auto Corpus=qEnvironmentVariable("HC_RHWP_TEST_CORPUS");
        if(RuntimeRoot.isEmpty() || Corpus.isEmpty()) QSKIP("Qualified runtime and corpus required");
        QFile Inventory(RuntimeRoot+"/runtime-inventory.json");QVERIFY(Inventory.open(QIODevice::ReadOnly));
        WebProcessRuntime Runtime;Runtime.root_=RuntimeRoot;
        Runtime.approvedInventorySha256_=QCryptographicHash::hash(Inventory.readAll(),QCryptographicHash::Sha256).toHex();
        QFile Input(Corpus+"/"+Name);QVERIFY(Input.open(QIODevice::ReadOnly));const auto Bytes=Input.readAll();
        QTemporaryDir Dir;const auto Copy=Dir.filePath(Name);
        {QFile File(Copy);QVERIFY(File.open(QIODevice::WriteOnly));QCOMPARE(File.write(Bytes),Bytes.size());}
        RecoverySnapshot Record;Record.viewId_="rhwp";Record.document_.id_=std::string(32,'c');Record.document_.contentMode_=DocumentContentMode::ExternalModel;
        Record.document_.generation_=1;Record.document_.revision_=2;Record.document_.savedRevision_=1;
        Record.document_.model_={4,1,"edited"};Record.document_.savedModel_={4,0,"saved"};Record.document_.dirty_=true;
        Record.bytes_=Bytes.toStdString();Record.checksum_=QCryptographicHash::hash(Bytes,QCryptographicHash::Sha256).toHex().toStdString();
        Record.bundleSha256_=Runtime.approvedInventorySha256_->toStdString();Record.format_=Format.toStdString();Record.engineSchema_=RecoveryEngineSchema;
        HwpOpenWindow Window(Copy,Dir.filePath("cache"),Runtime,{},true);
        QVERIFY(Window.SetRecovery(Record));Window.show();Window.Open();
        QTRY_VERIFY_WITH_TIMEOUT(Window.Document() || !Window.Error().isEmpty(),15000);
        QVERIFY2(Window.Error().isEmpty(),qPrintable(Window.Error()));
        QTRY_VERIFY_WITH_TIMEOUT(!Window.Document()->IsBusy(),45000);
        const auto State=Window.Document()->Snapshot();
        QCOMPARE(State.lifecycle_,DocumentLifecycle::Ready);QVERIFY(State.dirty_);
        QVERIFY(!Window.Document()->Capabilities().save_);QVERIFY(Window.Document()->Capabilities().saveAs_);
        Window.Document()->RequestClose(CloseDisposition::Discard);QTRY_VERIFY_WITH_TIMEOUT(Window.IsClosed(),15000);
        {QFile File(Copy);QVERIFY(File.open(QIODevice::ReadOnly));QCOMPARE(File.readAll(),Bytes);}
    }
    void checkpointAndReopen_data() {
        QTest::addColumn<QString>("Name"); QTest::addColumn<QString>("Format");
        QTest::newRow("hwp") << QString("c02.hwp") << QString("hwp");
        QTest::newRow("hwpx") << QString("c05.hwpx") << QString("hwpx");
    }
    void checkpointAndReopen() {
        QFETCH(QString, Name); QFETCH(QString, Format);
        const auto Corpus = qEnvironmentVariable("HC_RHWP_TEST_CORPUS"); if (Corpus.isEmpty()) QSKIP("Pinned corpus required");
        QFile File(Corpus + "/" + Name); QVERIFY(File.open(QIODevice::ReadOnly)); const auto Bytes = File.readAll();
        const auto Pin = QByteArray(64,'a'); const auto Sha = QCryptographicHash::hash(Bytes,QCryptographicHash::Sha256).toHex();
        auto Backend = std::make_unique<FakeBackend>(); auto* Port = Backend.get(); Port->editorEnabled_ = true;
        const std::string Id(32,'b'); HwpSdiWindow Window(Id,{},std::move(Backend),true);
        QVERIFY(Window.Open()); Port->Finish(); QTRY_VERIFY(!Window.IsBusy());
        emit Port->ModelChanged(Port->binding_,ExternalModelRevision{1,1,"edited"}); QTRY_VERIFY(Window.Snapshot().dirty_);
        const auto Saved = Window.Snapshot().savedModel_; QSignalSpy Ready(&Window,&HwpSdiWindow::CheckpointPrepared);
        QVERIFY(Window.Checkpoint(Format,Pin)); QVERIFY(Window.IsBusy());
        QVERIFY(!Window.Save({SaveDisposition::SaveAs,"never-written.hwp"},Format));
        const auto Identity = Port->command_["exportIdentity"].toObject();
        const auto Send = [&](QJsonObject Message) {
            Message["version"]=1;Message["identity"]=Identity;emit Port->EditorReply(Port->binding_,{{"message",Message}});
        };
        Send({{"type","ExportBegin"},{"format",Format},{"byteLength",Bytes.size()},{"sha256",QString::fromLatin1(Sha)},
            {"loss",QJsonObject{{"schemaVersion",1},{"outputFormat",Format},{"count",0},{"losses",QJsonArray{}}}}});
        for(qsizetype Offset=0;Offset<Bytes.size();Offset+=192*1024)
            Send({{"type","ExportChunk"},{"offset",Offset},{"data",QString::fromLatin1(Bytes.mid(Offset,192*1024).toBase64())}});
        Send({{"type","ExportCommit"}}); QTRY_COMPARE(Ready.size(),1);
        QVERIFY(Window.Snapshot().dirty_); QCOMPARE(Window.Snapshot().savedModel_,Saved); QVERIFY(!Window.IsBusy());
        QTemporaryDir Root; DocumentRecoveryStore Store(Root.path());
        auto Stored=Store.Store(Ready[0][0].value<RecoverySnapshot>()); QVERIFY(Stored.HasValue());
        auto Read=Store.Read(Stored.Value().snapshotPath_); QVERIFY(Read.HasValue()); QCOMPARE(Read.Value().bytes_,Bytes.toStdString());
        SourceIdentity Source; Source.inputSha256_=Sha.toStdString(); Source.localPath_="recovery-copy";
        auto RestoredBackend=std::make_unique<FakeBackend>();auto* RestoredPort=RestoredBackend.get();RestoredPort->editorEnabled_=true;
        HwpSdiWindow Restored(Id,Source,std::move(RestoredBackend),true);
        auto Bad=Read.Value();Bad.checksum_=std::string(64,'0');QVERIFY(!Restored.SetRecovery(Bad,Pin,Format));
        QVERIFY(!Restored.SetRecovery(Read.Value(),QByteArray(64,'c'),Format));
        Bad=Read.Value();Bad.document_.dirty_=false;QVERIFY(!Restored.SetRecovery(Bad,Pin,Format));
        Bad=Read.Value();Bad.document_.savedModel_.savepoint_="bad baseline with spaces";QVERIFY(!Restored.SetRecovery(Bad,Pin,Format));
        Bad=Read.Value();Bad.viewId_="text";QVERIFY(!Restored.SetRecovery(Bad,Pin,Format));
        QVERIFY(Restored.SetRecovery(Read.Value(),Pin,Format));QVERIFY(Restored.Open());
        emit RestoredPort->Loaded(RestoredPort->binding_,RestoredPort->ticket_,ExternalModelRevision{2,0,Sha.toStdString()});
        QTRY_VERIFY(!Restored.IsBusy());QCOMPARE(Restored.Snapshot().lifecycle_,DocumentLifecycle::Ready);
        QVERIFY(Restored.Snapshot().dirty_);QVERIFY(!Restored.Capabilities().save_);QVERIFY(Restored.Capabilities().saveAs_);
        QVERIFY(Restored.Snapshot().source_.sourceToken_.empty());
        Window.RequestClose(CloseDisposition::Discard);Port->AckStop();QTRY_COMPARE(Window.Snapshot().lifecycle_,DocumentLifecycle::Closed);
        Restored.RequestClose(CloseDisposition::Discard);RestoredPort->AckStop();QTRY_COMPARE(Restored.Snapshot().lifecycle_,DocumentLifecycle::Closed);
    }
    void checkpointFailureRetainsDirty() {
        auto Backend=std::make_unique<FakeBackend>();auto* Port=Backend.get();Port->editorEnabled_=true;
        HwpSdiWindow Window(std::string(32,'d'),{},std::move(Backend),true);
        QVERIFY(Window.Open());Port->Finish();QTRY_VERIFY(!Window.IsBusy());
        emit Port->ModelChanged(Port->binding_,ExternalModelRevision{1,1,"edited"});QTRY_VERIFY(Window.Snapshot().dirty_);
        const auto Saved=Window.Snapshot().savedModel_;
        QSignalSpy Failure(&Window,&HwpSdiWindow::CheckpointFailed),Ready(&Window,&HwpSdiWindow::CheckpointPrepared),SaveFailure(&Window,&HwpSdiWindow::SaveFailed);
        QVERIFY(Window.Checkpoint("hwp",QByteArray(64,'a')));
        emit Port->EditorReply(Port->binding_,{{"editor",QJsonObject{{"error","rhwp-edit-export-stale"}}}});
        QTRY_COMPARE(Failure.size(),1);QTRY_VERIFY(!Window.IsBusy());
        QCOMPARE(Ready.size(),0);QCOMPARE(SaveFailure.size(),0);QVERIFY(Window.Snapshot().dirty_);QCOMPARE(Window.Snapshot().savedModel_,Saved);
        Window.RequestClose(CloseDisposition::Discard);Port->AckStop();QTRY_COMPARE(Window.Snapshot().lifecycle_,DocumentLifecycle::Closed);
    }
    void closeBarrierWaitsForUnreportedChanges()
    {
        const auto Corpus = qEnvironmentVariable("HC_RHWP_TEST_CORPUS");
        if (Corpus.isEmpty()) QSKIP("Pinned input required");
        QTemporaryDir Dir; WebProcessRuntime Runtime; Runtime.approvedInventorySha256_ = QByteArray(64, 'a');
        QPointer<FakeBackend> Port;
        HwpOpenWindow Window(Corpus + "/c02.hwp", Dir.filePath("cache"), Runtime, [&]() -> std::unique_ptr<HwpSdiBackend> {
            auto Backend = std::make_unique<FakeBackend>(); Backend->editorEnabled_ = true; Backend->holdSync_ = true;
            Port = Backend.get(); return Backend;
        }, true);
        Window.Open(); QTRY_VERIFY(Port); Port->Finish(); QTRY_VERIFY(!Window.Document()->IsBusy());
        Window.RequestClose(); QCOMPARE(Port->stops_, 0);
        QCOMPARE(Port->command_["type"].toString(), QString("DocumentEditorSynchronize"));
        const auto Id = Port->command_["id"];
        emit Port->ModelChanged(Port->binding_, ExternalModelRevision{1, 1, "last-keystroke"});
        bool Asked = false; QTimer Cancel; Cancel.setInterval(10);
        connect(&Cancel, &QTimer::timeout, this, [&] {
            if (auto* Box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                Asked = true; Box->button(QMessageBox::Cancel)->click();
            }
        }); Cancel.start();
        emit Port->EditorReply(Port->binding_, {{"editor", QJsonObject{{"synchronized", Id}}}});
        QTRY_VERIFY(Asked); Cancel.stop(); QTRY_VERIFY(!Window.Document()->IsBusy());
        QVERIFY(Window.Document()->Snapshot().dirty_); QCOMPARE(Port->stops_, 0); QVERIFY(!Window.IsClosing());
        Window.Document()->RequestClose(CloseDisposition::Discard); Port->AckStop(); QTRY_VERIFY(Window.IsClosed());
    }
    void saveRejectionObserverMayDeleteOwner_data() {
        QTest::addColumn<bool>("SaveAs"); QTest::newRow("original") << false; QTest::newRow("save-as") << true;
    }
    void saveRejectionObserverMayDeleteOwner() {
        QFETCH(bool, SaveAs);
        const auto Corpus = qEnvironmentVariable("HC_RHWP_TEST_CORPUS");
        if (Corpus.isEmpty()) QSKIP("Pinned input required");
        QTemporaryDir Dir; WebProcessRuntime Runtime; Runtime.approvedInventorySha256_ = QByteArray(64,'a');
        QPointer<FakeBackend> Port;
        QPointer<HwpOpenWindow> Window = new HwpOpenWindow(Corpus + "/c02.hwp", Dir.filePath("cache"), Runtime,
            [&]() -> std::unique_ptr<HwpSdiBackend> { auto Backend = std::make_unique<FakeBackend>(); Port = Backend.get(); return Backend; }, true);
        Window->show(); Window->Open(); QTRY_VERIFY(Port); Port->Finish(); QTRY_VERIFY(!Window->Document()->IsBusy());
        connect(Window, &HwpOpenWindow::StateChanged, this, [&] {
            if (Window && Window->Document()->Snapshot().error_) delete Window.data();
        });
        const auto Original = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs,true);
        const auto Restore = qScopeGuard([&] { QApplication::setAttribute(Qt::AA_DontUseNativeDialogs,Original); });
        QTimer Choose; Choose.setInterval(20);
        connect(&Choose, &QTimer::timeout, this, [&] {
            if (auto* Dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget())) {
                Dialog->selectFile(Dir.filePath("copy.hwp")); QMetaObject::invokeMethod(Dialog,"accept",Qt::QueuedConnection);
            }
        }); Choose.start(); Window->Save(SaveAs); QTRY_VERIFY(!Window); Choose.stop();
        QVERIFY(!QFile::exists(Dir.filePath("copy.hwp")));
    }
    void closeBarrierTimeoutRequiresExplicitDiscard()
    {
        auto Backend = std::make_unique<FakeBackend>(); auto* Port = Backend.get();
        Port->editorEnabled_ = true; Port->holdSync_ = true;
        HwpSdiWindow Window("timeout", {}, std::move(Backend), true);
        QVERIFY(Window.Open()); Port->Finish(); QTRY_VERIFY(!Window.IsBusy());
        QVERIFY(Window.SynchronizeForClose()); QSignalSpy Timeout(&Window, &HwpSdiWindow::ControlTimeout);
        QTRY_COMPARE_WITH_TIMEOUT(Timeout.size(), 1, 32000);
        QVERIFY(Window.ControlTimedOut()); QCOMPARE(Port->stops_, 0);
        Window.RetryControl(); QVERIFY(!Window.ControlTimedOut()); QCOMPARE(Port->stops_, 0);
        Window.DiscardAndClose(); QCOMPARE(Port->stops_, 1); QVERIFY(Window.IsBusy());
        Port->AckStop(); QTRY_COMPARE(Window.Snapshot().lifecycle_, DocumentLifecycle::Closed);
    }
    void exportFailureObserverMayDeleteWindow()
    {
        auto Backend = std::make_unique<FakeBackend>(); auto* Port = Backend.get(); Port->editorEnabled_ = true;
        QPointer<HwpSdiWindow> Window = new HwpSdiWindow("delete", {}, std::move(Backend), true);
        QVERIFY(Window->Open()); Port->Finish(); QTRY_VERIFY(!Window->IsBusy());
        QVERIFY(Window->Save({SaveDisposition::SaveAs, "unused.hwp"}, "hwp"));
        connect(Window, &HwpSdiWindow::SaveFailed, this, [&] { delete Window.data(); });
        Port->AckStop(); QTRY_VERIFY(!Window);
    }
    void editExportFailurePreservesDirtyAndCloseGuard()
    {
        auto A = std::make_unique<FakeBackend>(); auto* Port = A.get(); Port->editorEnabled_ = true;
        HwpSdiWindow Window("edit", {}, std::move(A), true);
        QVERIFY(Window.Open()); Port->Finish(); QTRY_VERIFY(!Window.IsBusy());
        emit Port->ModelChanged(Port->binding_, ExternalModelRevision{1, 1, "changed"});
        QTRY_VERIFY(Window.Snapshot().dirty_);
        Window.RequestClose(); QVERIFY(!Window.IsClosing()); QCOMPARE(Port->stops_, 0);
        QSignalSpy Failure(&Window, &HwpSdiWindow::SaveFailed);
        QVERIFY(Window.Save({SaveDisposition::SaveAs, "new.hwp"}, "hwp")); QVERIFY(Window.IsBusy());
        emit Port->EditorReply(Port->binding_, QJsonObject{{"requestType", "DocumentEditorExport"}, {"ok", false}});
        QTRY_COMPARE(Failure.size(), 1); QTRY_VERIFY(!Window.IsBusy()); QVERIFY(Window.Snapshot().dirty_);
        QCOMPARE(Port->command_["type"].toString(), QString("DocumentEditorDiscard"));
        QVERIFY(Window.Save({SaveDisposition::SaveAs, "retry.hwp"}, "hwp"));
        emit Port->EditorReply(Port->binding_, QJsonObject{{"editor", QJsonObject{{"error", "rhwp-edit-export-stale"}}}});
        QTRY_COMPARE(Failure.size(), 2); QTRY_VERIFY(!Window.IsBusy()); QVERIFY(Window.Snapshot().dirty_);
        Window.RequestClose(CloseDisposition::Discard); QCOMPARE(Port->stops_, 1);
        Port->AckStop(); QTRY_COMPARE(Window.Snapshot().lifecycle_, DocumentLifecycle::Closed);
    }
    void cancellationAndStaleCompletion()
    {
        auto A = std::make_unique<FakeBackend>(); auto* Port = A.get();
        HwpSdiWindow Window("a", {}, std::move(A));
        QVERIFY(Window.Open()); const auto Old = Port->ticket_;
        Window.Cancel(); Window.Cancel();
        QCOMPARE(Port->cancels_, 1); QVERIFY(Window.IsBusy()); QVERIFY(!Window.Open());
        Port->AckCancel(); QTRY_VERIFY(!Window.IsBusy());
        QVERIFY(Window.Open());
        emit Port->Loaded(Port->binding_, Old, ExternalModelRevision{1, 0, "old"});
        emit Port->Loaded(Port->binding_ + 1, Port->ticket_, ExternalModelRevision{1, 0, "wrong"});
        QCoreApplication::processEvents();
        QVERIFY(Window.IsBusy());
        Port->Finish(); QTRY_VERIFY(!Window.IsBusy());
        QCOMPARE(Window.Snapshot().model_.savepoint_, std::string("base"));
        Window.RequestClose(); Port->AckStop(); QTRY_VERIFY(!Window.IsBusy());
    }
    void closeWaitsForBackendNotJustLoad()
    {
        auto A = std::make_unique<FakeBackend>(); auto* Port = A.get();
        HwpSdiWindow Window("a", {}, std::move(A)); QSignalSpy Closed(&Window, &HwpSdiWindow::Closed);
        Window.show(); QVERIFY(Window.Open());
        Window.close(); QTRY_VERIFY(Window.IsClosing());
        QVERIFY(Window.isVisible()); QVERIFY(Window.IsBusy());
        Window.RequestClose(); QCOMPARE(Port->stops_, 1); QCOMPARE(Port->cancels_, 1);
        Port->Finish(); QCoreApplication::processEvents();
        QVERIFY(Window.IsBusy()); QCOMPARE(Closed.count(), 0);
        emit Port->Stopped(Port->binding_ + 1); QCoreApplication::processEvents();
        QVERIFY(Window.IsBusy());
        Port->AckStop(); QTRY_COMPARE(Closed.count(), 1);
        QVERIFY(!Window.isVisible()); QVERIFY(!Window.Open());
        Port->AckStop(); QCoreApplication::processEvents(); QCOMPARE(Closed.count(), 1);
    }
    void stopRetiresPendingLease()
    {
        auto A = std::make_unique<FakeBackend>(); auto* Port = A.get();
        auto Hold = std::make_shared<Lease>(); std::weak_ptr<Lease> Weak = Hold;
        SourceIdentity Source; Source.lease_ = Hold; Hold.reset();
        HwpSdiWindow Window("a", std::move(Source), std::move(A));
        QVERIFY(Window.Open()); Window.RequestClose();
        QVERIFY(!Weak.expired()); QVERIFY(Window.IsBusy());
        Port->AckStop(); QTRY_VERIFY(!Window.IsBusy());
        QVERIFY(Weak.expired());
        QCOMPARE(Window.Snapshot().lifecycle_, DocumentLifecycle::Closed);
    }
    void crashAndFailure()
    {
        auto A = std::make_unique<FakeBackend>(); auto* Port = A.get();
        HwpSdiWindow Window("a", {}, std::move(A));
        QVERIFY(Window.Open());
        emit Port->Loaded(Port->binding_, Port->ticket_, ExternalModelRevision{0, 0, ""});
        QTRY_VERIFY(!Window.IsBusy()); QCOMPARE(Window.Snapshot().lifecycle_, DocumentLifecycle::Failed);
        QVERIFY(Window.Open()); Port->Finish(); QTRY_VERIFY(!Window.IsBusy());
        emit Port->Stopped(Port->binding_+1);QTest::qWait(20);
        QCOMPARE(Window.Snapshot().lifecycle_,DocumentLifecycle::Ready); // Stale owner failure cannot affect this binding.
        QVERIFY(Window.Capabilities().view_);
        Port->AckStop(); QTRY_VERIFY(!Window.Capabilities().view_);
        QCOMPARE(Window.Snapshot().lifecycle_, DocumentLifecycle::Failed);
        QVERIFY(!Window.Open()); Window.RequestClose();
        QCOMPARE(Window.Snapshot().lifecycle_, DocumentLifecycle::Closed);
    }
    void stoppedDuringLoad()
    {
        auto A = std::make_unique<FakeBackend>(); auto* Port = A.get();
        HwpSdiWindow Window("a", {}, std::move(A));
        QVERIFY(Window.Open()); Port->AckStop();
        QTRY_VERIFY(!Window.IsBusy());
        QCOMPARE(Window.Snapshot().lifecycle_, DocumentLifecycle::Failed);
        Window.RequestClose(); QCOMPARE(Window.Snapshot().lifecycle_, DocumentLifecycle::Closed);
    }
    void observerDeletesClosedWindow()
    {
        auto A = std::make_unique<FakeBackend>(); auto* Port = A.get();
        QPointer<HwpSdiWindow> Window = new HwpSdiWindow("a", {}, std::move(A));
        connect(Window, &HwpSdiWindow::StateChanged, this, [&Window] {
            if (Window && Window->Snapshot().lifecycle_ == DocumentLifecycle::Closed) delete Window.data();
        });
        Window->RequestClose(); Port->AckStop();
        QTRY_VERIFY(Window.isNull());
    }
    void destructionDropsQueuedCallbacks()
    {
        auto A = std::make_unique<FakeBackend>(); auto* Port = A.get();
        auto Window = std::make_unique<HwpSdiWindow>("a", SourceIdentity{}, std::move(A));
        QVERIFY(Window->Open()); Port->Finish();
        Window->RequestClose(); Port->AckStop();
        QTRY_COMPARE(Window->Snapshot().lifecycle_, DocumentLifecycle::Closed);
        Port->Finish(); Window.reset(); QCoreApplication::processEvents();
    }
};
QTEST_MAIN(HwpSdiTests)
#include "tstHwpSdiWindow.moc"
