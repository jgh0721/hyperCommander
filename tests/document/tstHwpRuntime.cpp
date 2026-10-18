#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "UniqueLibs/document/documentWebProcess.hpp"
#include <QtTest>
#include "UniqueLibs/document/hwpProcessBackend.hpp"
#include <QSignalSpy>
#include <QCryptographicHash>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QScopeGuard>
#include <future>
#include <atomic>
#include <thread>
using namespace hc::document;
class RuntimeTests final : public QObject
{
    Q_OBJECT
private:
    struct AttachedProbe
    {
        WebProcessSnapshot snapshot_;
        std::atomic<bool> ready_ = false, stop_ = false, exited_ = false;
    };
    std::future<QString> attach(const WebProcessRuntime& Runtime, HWND Parent, const std::shared_ptr<AttachedProbe>& Probe)
    {
        return std::async(std::launch::async, [Runtime, Parent, Probe] {
            WebProcessError Error;
            auto Owner = DocumentWebProcess::Start(Runtime, {4, 1024ULL*1024*1024, 10000},
                reinterpret_cast<std::uintptr_t>(Parent), Error);
            if(!Owner) { Probe->ready_.store(true, std::memory_order_release); return Error.code_; }
            Probe->snapshot_ = Owner->Snapshot();
            HANDLE Process = OpenProcess(SYNCHRONIZE, FALSE, Probe->snapshot_.processId_);
            QByteArray Reply;
            const bool StatusOk = Owner->Exchange(R"({"type":"RuntimeStatus","version":1})", Reply, 3000, Error)
                && QJsonDocument::fromJson(Reply).object().value("surfaceAttached") == QJsonValue(true);
            Probe->ready_.store(true, std::memory_order_release);
            QElapsedTimer Deadline; Deadline.start();
            while(Process && StatusOk && !Probe->stop_.load() && Deadline.elapsed() < 15000) {
                if(WaitForSingleObject(Process, 0) == WAIT_OBJECT_0) { Probe->exited_.store(true); break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            const bool Closed = Owner->Close();
            const bool Retired = Process && WaitForSingleObject(Process, 0) == WAIT_OBJECT_0;
            if(Process) CloseHandle(Process);
            if(!StatusOk) return QString("attached-status");
            return Closed && Retired ? QString() : QString("attached-retirement");
        });
    }
    bool fits(HWND Parent, HWND Child)
    {
        RECT A{}, B{};
        return GetClientRect(Parent, &A) && GetClientRect(Child, &B)
            && A.right == B.right && A.bottom == B.bottom;
    }
    WebProcessRuntime runtime()
    {
        WebProcessRuntime Runtime; Runtime.root_ = QString::fromUtf8(HC_TEST_RHWP_RUNTIME);
        QDirIterator Files(Runtime.root_, QDir::Files, QDirIterator::Subdirectories);
        while (Files.hasNext()) {
            const auto Path = Files.next();
            if (Path.endsWith(".log") || Path.endsWith(".lock")) continue;
            QFile File(Path); if (!File.open(QIODevice::ReadOnly)) return {};
            QCryptographicHash Hash(QCryptographicHash::Sha256);
            if (!Hash.addData(&File)) return {};
            Runtime.sha256_.insert(QDir(Runtime.root_).relativeFilePath(Path), Hash.result().toHex());
        }
        return Runtime;
    }
private slots:
    void unauthenticatedLaunchRejected()
    {
        QProcess Process;
        Process.start(QString::fromUtf8(HC_TEST_RHWP_RUNTIME) + "/HyperEditorWeb.exe",
            {"--WEBENGINEARGS", "--single-process"});
        QVERIFY(Process.waitForStarted(5000)); QVERIFY(Process.waitForFinished(5000));
        QCOMPARE(Process.exitCode(), 2);
    }
    void sdiColdRuntimeDoesNotGrantDocumentReadiness()
    {
        const auto Runtime = runtime(); QVERIFY(Runtime.sha256_.contains("HyperEditorWeb.exe"));
        auto Backend = std::make_unique<HwpProcessBackend>(Runtime, WebProcessLimits{4, 1024ULL*1024*1024, 10000});
        QSignalSpy Stopped(Backend.get(), &HwpSdiBackend::Stopped);
        HwpSdiWindow Window("real-helper", {}, std::move(Backend));
        QVERIFY(Window.Open()); QVERIFY(Window.IsBusy());
        QTRY_COMPARE_WITH_TIMEOUT(Stopped.count(), 1, 20000);
        QTRY_VERIFY(!Window.IsBusy()); QVERIFY(!Window.Capabilities().view_);
        QVERIFY(Window.Snapshot().error_.has_value());
        QCOMPARE(Window.Snapshot().error_->code_, std::string("rhwp-package-not-approved"));
        Window.RequestClose(); QTRY_COMPARE(Window.Snapshot().lifecycle_, DocumentLifecycle::Closed);
    }
    void authenticatedChildResizeAndParentDeath_data()
    {
        QTest::addColumn<QString>("Loss");
        QTest::newRow("parent-destroyed") << QString("parent");
        QTest::newRow("child-closed") << QString("child");
        QTest::newRow("same-process-other-parent") << QString("reparent");
    }
    void authenticatedChildResizeAndParentDeath()
    {
        QFETCH(QString, Loss);
        const auto Runtime = runtime(); QVERIFY(Runtime.sha256_.contains("HyperEditorWeb.exe"));
        auto Parent = std::make_unique<QWidget>(); Parent->resize(640, 360); Parent->show();
        const auto ParentWindow = reinterpret_cast<HWND>(Parent->winId());
        auto Probe = std::make_shared<AttachedProbe>(); auto Work = attach(Runtime, ParentWindow, Probe);
        const auto StopProbe = qScopeGuard([&] { Probe->stop_.store(true); });
        QTRY_VERIFY_WITH_TIMEOUT(Probe->ready_.load(std::memory_order_acquire), 15000);
        QVERIFY(Probe->snapshot_.authenticated_); QCOMPARE(Probe->snapshot_.activeProcesses_, 1U);
        const auto Child = reinterpret_cast<HWND>(Probe->snapshot_.childWindow_); QVERIFY(Child);
        DWORD Pid = 0; GetWindowThreadProcessId(Child, &Pid); QCOMPARE(Pid, Probe->snapshot_.processId_);
        QCOMPARE(GetParent(Child), ParentWindow); QVERIFY(GetWindowLongPtrW(Child, GWL_STYLE) & WS_CHILD);
        QVERIFY(!(GetWindowLongPtrW(Child, GWL_STYLE) & WS_VISIBLE));
        QTRY_VERIFY(fits(ParentWindow, Child));
        Parent->resize(817, 493);
        // Both native rectangles can still have the old matching size until Qt
        // dispatches the resize. Wait for growth as well as parent/child equality.
        RECT Rect{};
        QTRY_VERIFY(GetClientRect(Child, &Rect) && Rect.right > 640 && fits(ParentWindow, Child));
        QWidget OtherParent;
        if(Loss == "parent") Parent.reset();
        else if(Loss == "child") QVERIFY(PostMessageW(Child, WM_CLOSE, 0, 0));
        else {
            const auto OtherWindow = reinterpret_cast<HWND>(OtherParent.winId());
            QCOMPARE(SetParent(Child, OtherWindow), ParentWindow);
        }
        QTRY_VERIFY_WITH_TIMEOUT(Probe->exited_.load(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(Work.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 5000);
        const auto Error = Work.get(); QVERIFY2(Error.isEmpty(), qPrintable(Error));
        QVERIFY(!IsWindow(Child));
    }
    void attachedHelpersRemainIndependent()
    {
        const auto Runtime = runtime(); QVERIFY(Runtime.sha256_.contains("HyperEditorWeb.exe"));
        QWidget A, B; A.resize(400, 260); B.resize(730, 420); A.show(); B.show();
        const auto ParentA = reinterpret_cast<HWND>(A.winId()), ParentB = reinterpret_cast<HWND>(B.winId());
        auto First = std::make_shared<AttachedProbe>(), Second = std::make_shared<AttachedProbe>();
        auto WorkA = attach(Runtime, ParentA, First), WorkB = attach(Runtime, ParentB, Second);
        const auto StopProbes = qScopeGuard([&] { First->stop_.store(true); Second->stop_.store(true); });
        QTRY_VERIFY_WITH_TIMEOUT(First->ready_.load(std::memory_order_acquire) && Second->ready_.load(std::memory_order_acquire), 15000);
        QVERIFY(First->snapshot_.authenticated_ && Second->snapshot_.authenticated_);
        QVERIFY(First->snapshot_.processId_ != Second->snapshot_.processId_);
        const auto ChildA = reinterpret_cast<HWND>(First->snapshot_.childWindow_);
        const auto ChildB = reinterpret_cast<HWND>(Second->snapshot_.childWindow_);
        QCOMPARE(GetParent(ChildA), ParentA); QCOMPARE(GetParent(ChildB), ParentB);
        QVERIFY(!IsChild(ParentA, ChildB)); QVERIFY(!IsChild(ParentB, ChildA));
        First->stop_.store(true);
        QTRY_VERIFY_WITH_TIMEOUT(WorkA.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 5000);
        const auto ErrorA = WorkA.get(); QVERIFY2(ErrorA.isEmpty(), qPrintable(ErrorA));
        QVERIFY(!IsWindow(ChildA)); QVERIFY(IsWindow(ChildB)); QVERIFY(!Second->exited_.load());
        B.resize(910, 520); QTRY_VERIFY(fits(ParentB, ChildB));
        Second->stop_.store(true);
        QTRY_VERIFY_WITH_TIMEOUT(WorkB.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 5000);
        const auto ErrorB = WorkB.get(); QVERIFY2(ErrorB.isEmpty(), qPrintable(ErrorB));
        QVERIFY(!IsWindow(ChildB));
    }
    void authenticatedColdRuntime()
    {
        const auto Runtime = runtime(); QVERIFY(Runtime.sha256_.contains("HyperEditorWeb.exe"));
        // The ordinary GUI owner must never wait on launch/exchange/close.
        // Execute the complete owner lifetime on one serialized worker.
        auto Work = std::async(std::launch::async, [Runtime] {
            QString Failure; WebProcessError Error;
            auto Owner = DocumentWebProcess::Start(Runtime, {4, 1024ULL * 1024 * 1024, 10000}, 0, Error);
            if (!Owner) return Error.code_;
            const auto Pid = Owner->Snapshot().processId_;
            HANDLE Process = OpenProcess(SYNCHRONIZE, FALSE, Pid);
            auto Finish = [&](const QString& Problem) {
                const bool Closed = Owner->Close();
                const bool Retired = Process && WaitForSingleObject(Process, 5000) == WAIT_OBJECT_0;
                if (Process) CloseHandle(Process);
                if (!Problem.isEmpty()) return Problem;
                return Closed && Retired ? QString() : QString("runtime-not-retired");
            };
            QByteArray Reply;
            if (!Owner->Exchange(R"({"type":"RuntimeStatus","version":1})", Reply, 3000, Error)) return Finish(Error.code_);
            const auto Status = QJsonDocument::fromJson(Reply).object();
            if (Status.value("transportReady") != QJsonValue(true)
                || Status.value("packageApproved") != QJsonValue(false)
                || Status.value("rendererCreated") != QJsonValue(false)
                || Status.value("documentCommands") != QJsonValue(false)
                || Status.value("packageError") != QJsonValue("rhwp-package-policy-unavailable"))
                return Finish("runtime-status");
            for(const auto* Command : {"InputBegin", "InputChunk", "InputCommit", "InputCancel", "DocumentStart", "DocumentStatus"}) {
                const auto Request = QJsonDocument(QJsonObject{{"type", Command}, {"version", 1}, {"identity", QJsonObject{}}}).toJson(QJsonDocument::Compact);
                if(!Owner->Exchange(Request, Reply, 3000, Error)
                    || QJsonDocument::fromJson(Reply).object()["error"] != QJsonValue("rhwp-input-package-not-approved"))
                    return Finish("input-package-gate");
            }
            if (Owner->Snapshot().activeProcesses_ != 1) return Finish("cold-runtime-created-child");
            if (!Owner->Exchange(R"({"type":"Open","version":1})", Reply, 3000, Error)
                || QJsonDocument::fromJson(Reply).object().value("error") != QJsonValue("rhwp-command-unavailable"))
                return Finish("document-command-not-denied");
            if (!Owner->Exchange(R"({"type":"RuntimeStatus","version":1,"path":"untrusted"})", Reply, 3000, Error)
                || QJsonDocument::fromJson(Reply).object().value("error") != QJsonValue("rhwp-control-schema"))
                return Finish("control-schema-not-denied");
            if (!Owner->Exchange(R"({"type":"Shutdown","version":1})", Reply, 3000, Error)
                || QJsonDocument::fromJson(Reply).object().value("type") != QJsonValue("ShutdownAck"))
                return Finish("shutdown-ack");
            if (!Process || WaitForSingleObject(Process, 5000) != WAIT_OBJECT_0) return Finish("graceful-shutdown-timeout");
            return Finish({});
        });
        const auto Error = Work.get(); QVERIFY2(Error.isEmpty(), qPrintable(Error));
    }
};
QTEST_MAIN(RuntimeTests)
#include "tstHwpRuntime.moc"
