#include "pluginProcess.hpp"
#include "HyperCommanderBuiltInPlugin/SevenZip/builtinSevenZip.hpp"
#include "pluginHost/approvedWcxBackend.hpp"
#include "pipeChannel.hpp"
#include "hostedViewerSession.hpp"
#include "UniqueLibs/nativeMaterializer.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include "pluginHost/manifestFile.hpp"
#include <QJsonArray>
#include "memoryStorage.hpp"
#include "UniqueLibs/qtPluginViewerSurface.hpp"
#include "UniqueLibs/qtViewerHost.hpp"
#include <QAction>
#include <QBuffer>
#include <QDialog>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QtTest>
#include <QApplication>
#include <QWidget>
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QScreen>
#include <QScopeGuard>
#include <QTimer>
#include <future>
#include <iostream>
#include <atomic>
#include <cstring>
#include <thread>
#include <QVBoxLayout>
#include <QDateTime>
#include <QSet>
#include <oleacc.h>
#include <wrl/client.h>
#include <functional>

using namespace hc::plugins;
using namespace hc::storage;
namespace
{
    class TimedApplication final : public QApplication
    {
    public:
        using QApplication::QApplication;
        bool notify(QObject* Receiver, QEvent* Event) override
        {
            const auto Type = Event->type(); const QByteArray Class = Receiver->metaObject()->className();
            QElapsedTimer Timer; Timer.start(); const auto Result = QApplication::notify(Receiver, Event);
            if (Timer.elapsed() >= 100) qWarning() << "Slow Qt event" << Class << Type << "ms" << Timer.elapsed();
            return Result;
        }
    };
    template<class F> auto Worker(F&& Function)
    {
        auto Future = std::async(std::launch::async, std::forward<F>(Function));
        while (Future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) QTest::qWait(10);
        return Future.get();
    }
    HWND Window(const QJsonObject& Reply, const char* Key)
    { return reinterpret_cast<HWND>(static_cast<quintptr>(Reply[Key].toString().toULongLong(nullptr, 16))); }
    QString Title(HWND Window)
    { wchar_t Buffer[256]{}; GetWindowTextW(Window, Buffer, 256); return QString::fromWCharArray(Buffer); }
    QString NativeClass(HWND Window)
    { wchar_t Buffer[128]{}; GetClassNameW(Window, Buffer, 128); return QString::fromWCharArray(Buffer); }
    QString NativeText(HWND Window)
    {
        wchar_t Buffer[512]{}; DWORD_PTR Length = 0;
        if (!SendMessageTimeoutW(Window, WM_GETTEXT, std::size(Buffer), reinterpret_cast<LPARAM>(Buffer),
            SMTO_BLOCK | SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 500, &Length)) return {};
        return QString::fromWCharArray(Buffer, int(std::min<DWORD_PTR>(Length, std::size(Buffer) - 1)));
    }
    Result<LRESULT> NativeMessage(HWND Window, UINT Message, WPARAM WParam = 0, LPARAM LParam = 0)
    {
        DWORD_PTR Value = 0; SetLastError(0);
        if (!SendMessageTimeoutW(Window, Message, WParam, LParam, SMTO_BLOCK | SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 1000, &Value))
            return HostError("test-native-message", GetLastError());
        return static_cast<LRESULT>(Value);
    }
    QByteArray FileHash(const QString& Path)
    { QFile File(Path); if (!File.open(QIODevice::ReadOnly)) return {}; QCryptographicHash Hash(QCryptographicHash::Sha256); return Hash.addData(&File) ? Hash.result().toHex() : QByteArray{}; }
    Result<QJsonObject> HostPeFlags(const QString& Path)
    {
        QFile File(Path); if (!File.open(QIODevice::ReadOnly)) return HostError("test-host-pe-open"); const auto Bytes = File.readAll();
        IMAGE_DOS_HEADER Dos{}; IMAGE_NT_HEADERS64 Nt{};
        if (Bytes.size() < sizeof(Dos)) return HostError("test-host-pe-dos"); std::memcpy(&Dos, Bytes.constData(), sizeof(Dos));
        if (Dos.e_magic != IMAGE_DOS_SIGNATURE || Dos.e_lfanew < 0 || std::uint64_t(Dos.e_lfanew) + sizeof(Nt) > std::uint64_t(Bytes.size())) return HostError("test-host-pe-offset");
        std::memcpy(&Nt, Bytes.constData() + Dos.e_lfanew, sizeof(Nt));
        if (Nt.Signature != IMAGE_NT_SIGNATURE || Nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return HostError("test-host-pe-nt");
        const auto Flags = Nt.OptionalHeader.DllCharacteristics;
        return QJsonObject{{"highEntropy", bool(Flags & IMAGE_DLLCHARACTERISTICS_HIGH_ENTROPY_VA)}, {"dynamicBase", bool(Flags & IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE)},
            {"nxCompat", bool(Flags & IMAGE_DLLCHARACTERISTICS_NX_COMPAT)}, {"largeAddressAware", bool(Nt.FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE)}};
    }
    HWND TableCombo(HWND Viewer, const QString& Table)
    {
        struct Search { QString table_; HWND window_ = nullptr; } Value{Table};
        EnumChildWindows(Viewer, [](HWND Child, LPARAM Param) -> BOOL {
            auto& Value = *reinterpret_cast<Search*>(Param);
            if (!IsWindowVisible(Child) || !NativeClass(Child).contains("ComboBox", Qt::CaseInsensitive)) return TRUE;
            const auto Index = NativeMessage(Child, CB_FINDSTRINGEXACT, WPARAM(-1), reinterpret_cast<LPARAM>(Value.table_.utf16()));
            if (Index.HasValue() && Index.Value() != CB_ERR) { Value.window_ = Child; return FALSE; } return TRUE;
        }, reinterpret_cast<LPARAM>(&Value)); return Value.window_;
    }
    HWND VisibleGrid(HWND Viewer)
    {
        HWND Found = nullptr;
        EnumChildWindows(Viewer, [](HWND Child, LPARAM Param) -> BOOL {
            RECT Area{};
            if (IsWindowVisible(Child) && NativeClass(Child).contains("Grid", Qt::CaseInsensitive) && GetClientRect(Child, &Area) && Area.right >= 200 && Area.bottom >= 100) {
                *reinterpret_cast<HWND*>(Param) = Child; return FALSE;
            } return TRUE;
        }, reinterpret_cast<LPARAM>(&Found)); return Found;
    }
    Result<Unit> SelectTable(HWND Combo, const QString& Table)
    {
        auto Index = NativeMessage(Combo, CB_FINDSTRINGEXACT, WPARAM(-1), reinterpret_cast<LPARAM>(Table.utf16())); if (!Index.HasValue()) return Index.Failure();
        if (Index.Value() == CB_ERR) return HostError("test-sqlite-table-missing");
        auto Selected = NativeMessage(Combo, CB_SETCURSEL, WPARAM(Index.Value())); if (!Selected.HasValue()) return Selected.Failure();
        // CB_SETCURSEL deliberately does not notify. Execute the actual native
        // selection handler and wait for it to return before the next SDK call.
        auto Changed = NativeMessage(GetParent(Combo), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(Combo), CBN_SELCHANGE), reinterpret_cast<LPARAM>(Combo));
        if (!Changed.HasValue()) return Changed.Failure();
        const auto Current = NativeMessage(Combo, CB_GETCURSEL);
        if (!Current.HasValue() || Current.Value() != Index.Value()) return HostError("test-sqlite-table-selection"); return Unit{};
    }
    bool ClickRect(const RECT& Rect)
    {
        INPUT Events[3]{}; for (auto& Event : Events) Event.type = INPUT_MOUSE;
        Events[0].mi.dx = MulDiv((Rect.left + Rect.right) / 2 - GetSystemMetrics(SM_XVIRTUALSCREEN), 65535, GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1);
        Events[0].mi.dy = MulDiv((Rect.top + Rect.bottom) / 2 - GetSystemMetrics(SM_YVIRTUALSCREEN), 65535, GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1);
        Events[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        Events[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN; Events[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        return SendInput(3, Events, sizeof(INPUT)) == 3;
    }
    bool ClickWindow(HWND Window)
    { RECT Rect{}; return GetWindowRect(Window, &Rect) && ClickRect(Rect); }
    std::optional<RECT> AccessibleControl(HWND Viewer, const QString& Text)
    {
        // A native tab/toolbar item need not have its own HWND. MSAA supplies
        // the actual item's bounds, which are then clicked with real input.
        const auto Initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const auto Uninitialize = qScopeGuard([&] { if (SUCCEEDED(Initialized)) CoUninitialize(); });
        Microsoft::WRL::ComPtr<IAccessible> Root;
        if (FAILED(AccessibleObjectFromWindow(Viewer, OBJID_CLIENT, __uuidof(IAccessible), reinterpret_cast<void**>(Root.GetAddressOf())))) return {};
        unsigned Remaining = 1024;
        const auto Match = [&](IAccessible* Object, VARIANT Child) -> std::optional<RECT> {
            BSTR Name = nullptr; const auto Named = Object->get_accName(Child, &Name);
            const auto Caption = SUCCEEDED(Named) && Name ? QString::fromWCharArray(Name) : QString{}; SysFreeString(Name);
            if (!Caption.startsWith(Text)) return {};
            VARIANT State{}; const auto GotState = Object->get_accState(Child, &State);
            const bool Hidden = SUCCEEDED(GotState) && State.vt == VT_I4 && (State.lVal & (STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_OFFSCREEN)); VariantClear(&State);
            LONG X = 0, Y = 0, Width = 0, Height = 0;
            if (Hidden || FAILED(Object->accLocation(&X, &Y, &Width, &Height, Child)) || Width < 2 || Height < 2) return {};
            qInfo() << "Accessible settings item" << Caption << X << Y << Width << Height;
            return RECT{X, Y, X + Width, Y + Height};
        };
        std::function<std::optional<RECT>(IAccessible*, unsigned)> Find;
        Find = [&](IAccessible* Object, unsigned Depth) -> std::optional<RECT> {
            if (!Remaining || Depth > 8) return {}; --Remaining;
            VARIANT Self{}; Self.vt = VT_I4; Self.lVal = CHILDID_SELF;
            if (const auto Found = Match(Object, Self)) return Found;
            LONG Count = 0; if (FAILED(Object->get_accChildCount(&Count)) || Count < 1) return {};
            Count = std::min<LONG>(Count, std::min<LONG>(LONG(Remaining), 512));
            std::vector<VARIANT> Children(Count); LONG Obtained = 0;
            if (FAILED(AccessibleChildren(Object, 0, Count, Children.data(), &Obtained))) return {};
            const auto Clear = qScopeGuard([&] { for (auto& Child : Children) VariantClear(&Child); });
            for (LONG Index = 0; Index < Obtained && Remaining; ++Index) {
                auto& Child = Children[Index];
                if (Child.vt == VT_I4) { --Remaining; if (const auto Found = Match(Object, Child)) return Found; }
                else if (Child.vt == VT_DISPATCH && Child.pdispVal) {
                    Microsoft::WRL::ComPtr<IAccessible> Accessible;
                    if (SUCCEEDED(Child.pdispVal->QueryInterface(IID_PPV_ARGS(Accessible.GetAddressOf()))))
                        if (const auto Found = Find(Accessible.Get(), Depth + 1)) return Found;
                }
            }
            return {};
        };
        return Find(Root.Get(), 0);
    }
    void Activate(QWidget& Window)
    {
        Window.raise(); Window.activateWindow();
        const auto Foreground = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
        const auto Thread = GetCurrentThreadId();
        // Match the product UI harness: establish actual foreground ownership
        // before testing focus replay. Qt's cached focus alone is insufficient
        // when another desktop application holds the interactive input queue.
        const bool Attached = Foreground && Foreground != Thread && AttachThreadInput(Thread, Foreground, TRUE);
        SetForegroundWindow(reinterpret_cast<HWND>(Window.winId()));
        if (Attached) AttachThreadInput(Thread, Foreground, FALSE);
    }
    std::shared_ptr<IContentSource> MemoryContent(const QByteArray& Bytes, const std::string& Name = "문서.txt")
    {
        auto Provider = std::make_shared<test::MemoryProvider>();
        std::vector<std::byte> Data(Bytes.size()); if (!Data.empty()) std::memcpy(Data.data(), Bytes.constData(), Bytes.size());
        const auto Id = Provider->Add(Provider->Root(), Name, std::move(Data));
        return std::make_shared<ProviderContentSource>(Provider, Provider->Stat(Id, {}).get().Value());
    }
    QStringList Leases(const QString& Base)
    { return QDir(Base + "/hc-materialized-v1").entryList({"hc-lease-v1-*"}, QDir::Dirs | QDir::NoDotAndDotDot); }
    class WaitingContent final : public IContentSource
    {
    public:
        std::atomic<bool> reading_ = false;
        EntrySnapshot Describe() const override { return source_->Describe(); }
        Task<Unit> Verify(OperationContext Context) override { return source_->Verify(Context); }
        Task<std::shared_ptr<IReadStream>> OpenRead(OperationContext Context) override
        {
            reading_ = true;
            while (!Context.Check({"wlx-test"})) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return Ready<std::shared_ptr<IReadStream>>(*Context.Check({"wlx-test"}));
        }
    private:
        std::shared_ptr<IContentSource> source_ = MemoryContent("waiting");
    };
}
class HostedWlxTests : public QObject
{
    Q_OBJECT
private:
    QString host_ = QCoreApplication::applicationDirPath() + "/hc-plugin-host-x64.exe";
    QSet<QString> sqliteInstances_;
    bool prepare(QTemporaryDir& Directory, PluginApproval& Approval, bool Sdk = false)
    {
        if (!Directory.isValid() || !QFile::copy(QCoreApplication::applicationDirPath() + (Sdk ? "/hc_wlx_sdk_fixture.wlx64" : "/hc_wlx_fixture.wlx64"), Directory.filePath("test.wlx64"))) return false;
        QFile Binary(Directory.filePath("test.wlx64")); if (!Binary.open(QIODevice::ReadOnly)) return false;
        PluginManifest Value; Value.id_ = "hc.test.wlx"; Value.version_ = "1.0"; Value.binary_ = "test.wlx64";
        Value.format_ = PluginFormat::Wlx; Value.sha256_ = QCryptographicHash::hash(Binary.readAll(), QCryptographicHash::Sha256).toHex().toStdString();
        Value.extensions_ = {Extension::Viewer}; Value.permissions_ = {Permission::ReadContent, Permission::UserInterface};
        Approval.reviewed_ = Value; Approval.approved_ = true; Approval.granted_ = Value.permissions_;
        QFile Manifest(Directory.filePath("plugin.json")); return Manifest.open(QIODevice::WriteOnly) && Manifest.write(EncodeManifest(Value)) > 0;
    }
    QJsonObject request(QWidget& Parent, const QString& Path)
    {
        QFile File(Path); if (File.open(QIODevice::WriteOnly)) File.write("Unicode WLX content\n");
        return {{"parent", QString::number(Parent.winId(), 16)}, {"file", Path},
            {"width", qRound(Parent.width() * Parent.devicePixelRatioF())}, {"height", qRound(Parent.height() * Parent.devicePixelRatioF())}, {"flags", 0}};
    }
    PluginViewerOptions options(const QTemporaryDir& Directory, PluginApproval Approval)
    {
        PluginViewerOptions Options; Options.executable_ = host_; Options.manifestPath_ = Directory.filePath("plugin.json");
        Options.binaryPath_ = Directory.filePath("test.wlx64"); Options.approval_ = std::move(Approval);
        Options.limits_.requestMs_ = 500; Options.limits_.heartbeatMs_ = 200; return Options;
    }
private slots:
    void attachedInputHang_data()
    {
        QTest::addColumn<bool>("Detach"); QTest::newRow("job-recovery") << false; QTest::newRow("parent-detach") << true;
    }
    void attachedInputHang()
    {
        QFETCH(bool, Detach);
        QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, Approval));
        QWidget Parent; Parent.resize(420, 300); QPushButton Button("input probe", &Parent); Button.setGeometry(10, 10, 180, 35);
        QWidget Anchor(&Parent); Anchor.setGeometry(0, 60, 400, 220); Parent.show(); Anchor.show(); Button.show();
        QVERIFY(QTest::qWaitForWindowExposed(&Parent)); Activate(Parent);
        auto Started = Worker([&] { return PluginProcess::Start(host_, Directory.filePath("plugin.json"), Approval, options(Directory, Approval).limits_); }); QVERIFY(Started.HasValue());
        auto Host = std::move(Started.Value()); const auto Cleanup = qScopeGuard([&] { Worker([&] { Host.reset(); }); });
        const auto Open = request(Anchor, Directory.filePath("input.txt"));
        auto Opened = Worker([&] { return Host->ViewerCall(WireKind::ViewerOpen, Open); }); QVERIFY(Opened.HasValue()); const auto Viewer = Window(Opened.Value(), "viewer");
        const auto Focus = [&] { Activate(Parent); return Worker([&] { return Host->ViewerCall(WireKind::ViewerFocus, {}); }); };
        auto Focused = Focus(); QVERIFY(Focused.HasValue()); QVERIFY(Focused.Value()["focused"].toBool());
        QTRY_VERIFY_WITH_TIMEOUT(Host->Snapshot().runtime_["inputAttached"].toBool(), 1000);
        unsigned Clicks = 0; connect(&Button, &QPushButton::clicked, this, [&] { ++Clicks; });
        const auto Click = [&] { return ClickWindow(reinterpret_cast<HWND>(Button.winId())); };
        QVERIFY(Click()); QTRY_COMPARE_WITH_TIMEOUT(Clicks, 1u, 1000); Clicks = 0;
        Focused = Focus(); QVERIFY(Focused.HasValue()); QVERIFY(Focused.Value()["focused"].toBool());
        QElapsedTimer Timer; Timer.start(); qint64 Previous = 0, MaximumGap = 0; QTimer Beat; Beat.setInterval(10);
        connect(&Beat, &QTimer::timeout, this, [&] { const auto Now = Timer.elapsed(); MaximumGap = std::max(MaximumGap, Now - Previous); Previous = Now; }); Beat.start();
        QVERIFY(PostMessageW(Viewer, WM_APP + 81, 0, 0)); QTRY_COMPARE_WITH_TIMEOUT(GetDlgCtrlID(Viewer), 81, 1000);
        bool Detached = false; DWORD Error = 0; qint64 DetachMs = 0;
        if (Detach) {
            const auto ParentThread = GetCurrentThreadId(), HostThread = GetWindowThreadProcessId(Viewer, nullptr);
            Worker([&] { QElapsedTimer Cost; Cost.start(); SetLastError(0); Detached = AttachThreadInput(ParentThread, HostThread, FALSE); Error = GetLastError(); DetachMs = Cost.elapsed(); });
        }
        QElapsedTimer Input; Input.start(); QVERIFY(Click());
        QTRY_COMPARE_WITH_TIMEOUT(Clicks, 1u, 2000); const auto InputMs = Input.elapsed();
        QTRY_VERIFY_WITH_TIMEOUT(!Host->Snapshot().running_, 4000);
        qInfo() << "Attached input recovery" << "detach requested" << Detach << "success" << Detached << "error" << Error
            << "detach ms" << DetachMs << "input ms" << InputMs << "GUI gap ms" << MaximumGap << Host->Snapshot().runtime_;
        QVERIFY(MaximumGap < 200);
    }
    void externalArchiveClose_data()
    {
        QTest::addColumn<bool>("Discard"); QTest::addColumn<bool>("FailFirst"); QTest::addColumn<bool>("Edited");
        QTest::newRow("publish") << false << false << true;
        QTest::newRow("retry") << false << true << true;
        QTest::newRow("discard") << true << false << true;
        QTest::newRow("unchanged") << false << false << false;
    }
    void externalArchiveClose()
    {
        QFETCH(bool, Discard); QFETCH(bool, FailFirst); QFETCH(bool, Edited);
        QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, Approval));
        Approval.reviewed_.permissions_.insert(Permission::WriteContent); Approval.granted_ = Approval.reviewed_.permissions_;
        QFile Manifest(Directory.filePath("plugin.json")); QVERIFY(Manifest.open(QIODevice::WriteOnly)); Manifest.write(EncodeManifest(Approval.reviewed_)); Manifest.close();
        auto Source = std::dynamic_pointer_cast<ProviderContentSource>(MemoryContent("original bytes")); QVERIFY(Source);
        QByteArray Published; int Attempts = 0;
        Source->SetArchiveWriteback([&](const std::string& Path, OperationContext) -> Result<Unit> {
            if (++Attempts == 1 && FailFirst) return Error{ErrorCategory::Unavailable, "injected-archive-conflict"};
            QFile Draft(QString::fromStdString(Path)); if (!Draft.open(QIODevice::ReadOnly)) return Error{ErrorCategory::Unavailable, "draft-missing"};
            Published = Draft.readAll(); return Unit{};
        });
        QWidget Main; Main.show(); PluginViewerService Service([&] { return MakeNativeMaterializer(Directory.path().toStdString()); });
        const auto Drain = qScopeGuard([&] { Service.CancelAll(); while (!Service.IsIdle()) QTest::qWait(10); });
        auto Opened = Service.Open(options(Directory, Approval), Source, QString::number(Main.winId(), 16), 320, 200); QVERIFY(Opened.HasValue());
        auto Session = Opened.Value(); QTRY_COMPARE_WITH_TIMEOUT(Session->Snapshot().status_, hc::viewer::ViewerStatus::Ready, 8000);
        QVERIFY(Session->Details().externalArchive_); const auto Draft = Session->Details().externalArchiveDraft_;
        if (Edited) {
        auto Saved = Session->SendCommand(4, 77); QVERIFY(Saved.HasValue());
        QTRY_VERIFY_WITH_TIMEOUT(Session->Details().commandSequence_ >= Saved.Value(), 4000); QVERIFY(!Session->Details().commandError_);
        }
        QCOMPARE(Attempts, 0);
        auto Closed = Session->CloseArchive(); QVERIFY(Closed.HasValue());
        QTRY_VERIFY_WITH_TIMEOUT(Session->Details().commandSequence_ >= Closed.Value(), 4000);
        if (!Edited) { QVERIFY(Session->Details().externalArchiveDone_); Session->Close(); QTRY_VERIFY_WITH_TIMEOUT(Service.IsIdle(), 4000); QVERIFY(!QFileInfo::exists(Draft)); QCOMPARE(Attempts, 0); return; }
        QVERIFY(Session->Details().externalArchiveClosed_); QVERIFY(!Session->Details().externalArchiveDone_); QVERIFY(QFileInfo::exists(Draft));
        auto Commit = Session->CloseArchive(Discard ? 2 : 1); QVERIFY(Commit.HasValue());
        QTRY_VERIFY_WITH_TIMEOUT(Session->Details().commandSequence_ >= Commit.Value(), 4000);
        if (FailFirst) {
            QVERIFY(Session->Details().commandError_); QVERIFY(QFileInfo::exists(Draft));
            Commit = Session->CloseArchive(1); QVERIFY(Commit.HasValue());
            QTRY_VERIFY_WITH_TIMEOUT(Session->Details().commandSequence_ >= Commit.Value(), 4000);
        }
        QVERIFY2(!Session->Details().commandError_, "archive close must complete"); QVERIFY(Session->Details().externalArchiveDone_);
        QCOMPARE(Published, Discard ? QByteArray{} : QByteArray("saved by WLX"));
        Session->Close(); QTRY_VERIFY_WITH_TIMEOUT(Service.IsIdle(), 4000); QVERIFY(!QFileInfo::exists(Draft));
    }
    void nativeOriginalSave_data()
    {
        QTest::addColumn<bool>("Approved"); QTest::addColumn<bool>("Local"); QTest::addColumn<bool>("ReadOnly");
        QTest::newRow("approved-local") << true << true << false;
        QTest::newRow("unapproved-local") << false << true << false;
        QTest::newRow("approved-virtual") << true << false << false;
        QTest::newRow("approved-readonly") << true << true << true;
    }
    void nativeOriginalSave()
    {
        QFETCH(bool, Approved); QFETCH(bool, Local); QFETCH(bool, ReadOnly);
        QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, Approval));
        if (Approved) {
            Approval.reviewed_.permissions_.insert(Permission::WriteContent); Approval.granted_ = Approval.reviewed_.permissions_;
            QFile Manifest(Directory.filePath("plugin.json")); QVERIFY(Manifest.open(QIODevice::WriteOnly));
            QVERIFY(Manifest.write(EncodeManifest(Approval.reviewed_)) > 0); Manifest.close();
        }
        const auto Path = Directory.filePath("original.txt");
        QFile Original(Path); QVERIFY(Original.open(QIODevice::WriteOnly)); Original.write("original bytes"); Original.close();
        if (ReadOnly) QVERIFY(SetFileAttributesW(Path.toStdWString().c_str(), FILE_ATTRIBUTE_READONLY));
        const auto RestoreAttributes = qScopeGuard([&] { SetFileAttributesW(Path.toStdWString().c_str(), FILE_ATTRIBUTE_NORMAL); });
        auto Provider = MakeNativeReadProvider(LocatorKind::Local);
        auto Entry = Worker([&] { return Provider->Resolve({LocatorKind::Local, Path.toUtf8().toStdString()}, {}).get(); });
        QVERIFY(Entry.HasValue());
        std::shared_ptr<IContentSource> Source = Local ? std::make_shared<ProviderContentSource>(Provider, Entry.Value()) : MemoryContent("original bytes");
        QWidget Main; Main.resize(640, 360); Main.show();
        PluginViewerService Service([&] { return MakeNativeMaterializer(Directory.path().toStdString()); });
        const auto Drain = qScopeGuard([&] { Service.CancelAll(); while (!Service.IsIdle()) QTest::qWait(10); });
        auto Options = options(Directory, Approval); Options.purpose_ = hc::viewer::ViewerPurpose::QuickPreview;
        auto Opened = Service.Open(Options, Source, QString::number(Main.winId(), 16), 320, 200); QVERIFY(Opened.HasValue());
        auto Session = Opened.Value();
        QTRY_VERIFY_WITH_TIMEOUT(Session->Snapshot().status_ == hc::viewer::ViewerStatus::Ready || Session->Snapshot().status_ == hc::viewer::ViewerStatus::Failed, 8000);
        QVERIFY2(Session->Snapshot().status_ == hc::viewer::ViewerStatus::Ready, Session->Snapshot().error_ ? Session->Snapshot().error_->code_.c_str() : "not ready");
        QCOMPARE(Session->Details().directOriginal_, Approved && Local && !ReadOnly);
        auto Sent = Session->SendCommand(4, 77); QVERIFY(Sent.HasValue());
        QTRY_VERIFY_WITH_TIMEOUT(Session->Details().commandSequence_ >= Sent.Value(), 4000);
        QVERIFY(Original.open(QIODevice::ReadOnly));
        QCOMPARE(Original.readAll(), Approved && Local && !ReadOnly ? QByteArray("saved by WLX") : QByteArray("original bytes")); Original.close();
        Session->Close(); QTRY_VERIFY_WITH_TIMEOUT(Service.IsIdle(), 4000);
        QVERIFY(Leases(Directory.path()).isEmpty());
    }
    void builtinImageDirectInputAndPresentation()
    {
        const auto Package=QCoreApplication::applicationDirPath()+"/Addons/WLX/HyperViewer/hyperviewer.json";
        if(!QFileInfo::exists(Package))QSKIP("Build hc_hyperviewer_package for the built-in image integration test");
        auto Inspected=Worker([&]{return InspectManifestFile(Package);});QVERIFY(Inspected.HasValue());
        QTemporaryDir Root;const auto Path=Root.filePath("direct.png");QImage Image(2400,1800,QImage::Format_RGB32);Image.fill(Qt::cyan);QVERIFY(Image.save(Path));
        auto Provider=MakeNativeReadProvider(LocatorKind::Local);
        auto Entry=Worker([&]{return Provider->Resolve({LocatorKind::Local,Path.toUtf8().toStdString()},{}).get();});QVERIFY(Entry.HasValue());
        auto Source=std::make_shared<ProviderContentSource>(Provider,Entry.Value());
        std::atomic<int> Copies=0;QWidget Main;Main.resize(900,600);auto* Layout=new QVBoxLayout(&Main);
        QtPluginViewerController Controller(&Main,[&]{++Copies;return MakeNativeMaterializer(Root.path().toStdString());});
        auto* Surface=new QtPluginViewerSurface(&Controller,&Main);Layout->addWidget(Surface);Main.show();
        const auto Drain=qScopeGuard([&]{Surface->Close();while(!Controller.IsIdle())QTest::qWait(10);});
        PluginViewerOptions Options;Options.executable_=host_;Options.manifestPath_=Package;Options.binaryPath_=Inspected.Value().binaryPath_;
        Options.approval_={Inspected.Value().manifest_,true,false,Inspected.Value().manifest_.permissions_};Options.defaultIniPath_=Root.filePath("plugin.ini");
        Surface->Open(Options,Source);
        QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady() || Surface->Snapshot().status_==hc::viewer::ViewerStatus::Failed,20000);
        QVERIFY2(Surface->IsReady(),Surface->Snapshot().error_?Surface->Snapshot().error_->code_.c_str():"image not presented");
        QCOMPARE(Copies.load(),0);QVERIFY(Leases(Root.path()).isEmpty());
        const auto Documents=Surface->Details().documentState_["documents"].toArray();QCOMPARE(Documents.size(),1);
        QVERIFY(Documents[0].toObject()["imagePresentationReady"].toBool());
        // Borrowing a local image must not lock out external editors.
        QFile Writable(Path);QVERIFY(Writable.open(QIODevice::ReadWrite));Writable.close();
        QTRY_VERIFY_WITH_TIMEOUT(Surface->Details().documentState_["idle"].toBool(),10000);
        QVERIFY(Surface->OpenPrepared({{Source,{}}}));QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(),20000);
        QCOMPARE(Copies.load(),0);
    }
    void closedBookReleasesInputWhileViewerStaysAlive_data()
    {
        QTest::addColumn<QString>("Extension");
        for (const auto* Extension : {"zip", "cbz", "7z", "cb7"}) QTest::newRow(Extension) << QString::fromLatin1(Extension);
    }
    void closedBookReleasesInputWhileViewerStaysAlive()
    {
        QFETCH(QString, Extension);
        const auto Package = QCoreApplication::applicationDirPath() + "/Addons/WLX/HyperViewer/hyperviewer.json";
        QVERIFY(QFileInfo::exists(Package)); auto Inspected = Worker([&] { return InspectManifestFile(Package); }); QVERIFY(Inspected.HasValue());
        QTemporaryDir Root; QVERIFY(Root.isValid()); const auto Path = Root.filePath("closed-book." + Extension);
        const auto Bytes = QByteArray::fromBase64("UEsDBBQAAAAAAHFrYl2sgj2NSQAAAEkAAAAIAAAAcGFnZS5wbmeJUE5HDQoaCgAAAA1JSERSAAAAAgAAAAMIAgAAADaISdYAAAAQSURBVHicYzAoWABEDCgUAEw1B4FUiYMjAAAAAElFTkSuQmCCUEsBAhQAFAAAAAAAcWtiXayCPY1JAAAASQAAAAgAAAAAAAAAAAAAAIABAAAAAHBhZ2UucG5nUEsFBgAAAAABAAEANgAAAG8AAAAAAA==");
        if (Extension == "zip" || Extension == "cbz") {
            QFile File(Path); QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write(Bytes), Bytes.size());
        } else {
            { QFile File(Root.filePath("page.png")); QVERIFY(File.open(QIODevice::WriteOnly)); QVERIFY(File.write(QByteArray::fromBase64("iVBORw0KGgoAAAANSUhEUgAAAAIAAAADCAIAAAA2iEnWAAAAEElEQVR4nGMwKFgARAwoFABMNQeBVImDIwAAAABJRU5ErkJggg==")) > 0); }
            ArchiveUpdate Create{Root.filePath("created.7z").toStdString(), Root.path().toStdString(), {"page.png"}}; Create.creation_ = ArchiveCreationOptions{1, false};
            const auto Made = MakeApprovedWcxBackend(BuiltinSevenZipOptions())->Update(Create, {});
            QVERIFY2(Made.HasValue(), Made.HasValue() ? "" : Made.Failure().code_.c_str()); QVERIFY(QFile::rename(Root.filePath("created.7z"), Path));
        }
        auto Provider = MakeNativeReadProvider(LocatorKind::Local);
        auto Entry = Worker([&] { return Provider->Resolve({LocatorKind::Local, Path.toStdString()}, {}).get(); }); QVERIFY(Entry.HasValue());
        auto Source = std::make_shared<ProviderContentSource>(Provider, Entry.Value());
        QWidget Main; Main.resize(900, 600); auto* Layout = new QVBoxLayout(&Main);
        QtPluginViewerController Controller(&Main, [&] { return MakeNativeMaterializer(Root.path().toStdString()); });
        auto* Surface = new QtPluginViewerSurface(&Controller, &Main); Layout->addWidget(Surface); Main.show();
        const auto Drain = qScopeGuard([&] { Surface->Close(); while (!Controller.IsIdle()) QTest::qWait(10); });
        PluginViewerOptions Options; Options.executable_ = host_; Options.manifestPath_ = Package; Options.binaryPath_ = Inspected.Value().binaryPath_;
        Options.approval_ = {Inspected.Value().manifest_, true, false, Inspected.Value().manifest_.permissions_}; Options.defaultIniPath_ = Root.filePath("plugin.ini");
        Surface->Open(Options, Source);
        QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady() || Surface->Snapshot().status_ == hc::viewer::ViewerStatus::Failed, 20000);
        QVERIFY2(Surface->IsReady(), Surface->Snapshot().error_ ? Surface->Snapshot().error_->code_.c_str() : "book viewer not ready");
        QTRY_COMPARE_WITH_TIMEOUT(Surface->Details().documentState_["documents"].toArray().size(), 1, 10000);
        const auto Document = Surface->Details().documentState_["documents"].toArray()[0].toObject()["documentId"].toString();
        QVERIFY(!QFile::remove(Path)); // The open book must still own its input.
        const auto Closed = Surface->SendDocument({{"action", "close"}, {"documentId", Document}}); QVERIFY(Closed.HasValue());
        QTRY_VERIFY_WITH_TIMEOUT(Surface->Details().documentState_["documents"].toArray().isEmpty(), 10000);
        // A reusable viewer must not keep the initial document's separate pin.
        // Keep its process/session running to expose accidental extra ownership.
        QVERIFY(!Controller.IsIdle()); bool Removed = false; DWORD Code = ERROR_SUCCESS; QElapsedTimer Wait; Wait.start();
        const auto Native = QDir::toNativeSeparators(Path).toStdWString();
        while (!(Removed = DeleteFileW(Native.c_str())) && Wait.elapsed() < 3000) { Code = GetLastError(); QTest::qWait(20); }
        if (!Removed) qInfo() << "Closed book Windows error" << Code << "viewer pid" << Surface->Details().host_.processId_;
        QVERIFY(Removed);
    }
    void changedPackageRecovery()
    {
        QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, Approval));
        { QFile File(Directory.filePath("test.wlx64")); QVERIFY(File.open(QIODevice::Append)); QVERIFY(File.write("version-two") > 0); }
        QWidget Main; Main.show();
        QtViewerHost Host(&Main, [&] { return MakeNativeMaterializer(Directory.path().toStdString()); });
        QString Reviewed;
        Host.reviewPlugin_ = [&](const QString& Path) { Reviewed = Path; };
        auto* Dialog = Host.OpenPlugin(options(Directory, Approval), {{MemoryContent("test"), {}}}, "fixture"); QVERIFY(Dialog);
        QTRY_VERIFY_WITH_TIMEOUT(Dialog->findChild<QMessageBox*>("hc.plugin.changed"), 5000);
        auto* Notice = Dialog->findChild<QMessageBox*>("hc.plugin.changed");
        QVERIFY(Notice->text().contains("승인")); QVERIFY(Notice->detailedText().contains("plugin-binary-hash-mismatch"));
        Notice->findChild<QPushButton*>("hc.plugin.changedReview")->click();
        QCOMPARE(Reviewed, Directory.filePath("test.wlx64"));
        QTRY_VERIFY_WITH_TIMEOUT(Host.IsIdle(), 5000);
    }
    void compatibilityPackage()
    {
        auto ManifestPath = qEnvironmentVariable("HC_WLX_MANIFEST");
        QTemporaryDir Generated;
        if (const auto Binary = qEnvironmentVariable("HC_WLX_BINARY"); !Binary.isEmpty()) {
            auto Draft = Worker([&] { return InspectPluginBinary(Binary, PluginFormat::Wlx); });
            QVERIFY2(Draft.HasValue(), Draft.HasValue() ? "" : Draft.Failure().code_.c_str());
            auto Saved = Worker([&] { return SavePluginManifest(Draft.Value(), Generated.path()); }); QVERIFY(Saved.HasValue()); ManifestPath = Saved.Value();
        }
        if (ManifestPath.isEmpty()) QSKIP("Set HC_WLX_MANIFEST and HC_WLX_SOURCE for an explicitly selected compatibility package");
        const auto Source = qEnvironmentVariable("HC_WLX_SOURCE"); QVERIFY(QFileInfo::exists(Source));
        auto Inspected = Worker([&] { return InspectManifestFile(ManifestPath); });
        QVERIFY2(Inspected.HasValue(), Inspected.HasValue() ? "" : Inspected.Failure().code_.c_str());
        PluginApproval Approval{Inspected.Value().manifest_, true, false, Inspected.Value().manifest_.permissions_};
        QWidget Parent; Parent.resize(900, 600); Parent.show(); QVERIFY(QTest::qWaitForWindowExposed(&Parent)); Activate(Parent);
        qInfo() << "Compatibility DPR:" << Parent.devicePixelRatioF();
        const auto ExpectedDpr = qEnvironmentVariable("HC_VIEWER_EXPECT_DPR");
        if (!ExpectedDpr.isEmpty()) QCOMPARE(Parent.devicePixelRatioF(), ExpectedDpr.toDouble());
        QTemporaryDir Config;
        HostLimits Limits; const auto MemoryMb = qEnvironmentVariableIntValue("HC_WLX_MEMORY_MB");
        const auto MenuKey = qEnvironmentVariableIntValue("HC_WLX_MENU_KEY");
        const auto SettingsButton = qEnvironmentVariable("HC_WLX_SETTINGS_BUTTON_TEXT");
        if ((MenuKey > 0 && MenuKey <= 255) || !SettingsButton.isEmpty()) { Limits.requestMs_ = 500; Limits.heartbeatMs_ = 200; }
        if (MemoryMb > 0 && MemoryMb <= 4096) Limits.memoryBytes_ = std::uint64_t(MemoryMb) * 1024 * 1024;
        auto Started = Worker([&] { return PluginProcess::Start(host_, ManifestPath, Approval, Limits, {}, Config.filePath("lsplugin.ini")); });
        QVERIFY2(Started.HasValue(), Started.HasValue() ? "" : Started.Failure().code_.c_str());
        auto Host = std::move(Started.Value()); const auto Cleanup = qScopeGuard([&] { Worker([&] { Host.reset(); }); });
        const auto Probe = Worker([&] { return Host->Probe(); }); QVERIFY(Probe.HasValue()); qInfo() << "Compatibility probe" << Probe.Value();
        QJsonObject Request{{"parent", QString::number(Parent.winId(), 16)}, {"file", Source}, {"width", qRound(900 * Parent.devicePixelRatioF())}, {"height", qRound(600 * Parent.devicePixelRatioF())}, {"flags", 0}};
        auto Opened = Worker([&] { return Host->ViewerCall(WireKind::ViewerOpen, Request); });
        QVERIFY2(Opened.HasValue(), Opened.HasValue() ? "" : qPrintable(QString::fromStdString(Opened.Failure().code_) + " exit=" + QString::number(Host->Snapshot().exitCode_.value_or(0))));
        const auto Viewer = Window(Opened.Value(), "viewer"); QVERIFY(IsWindow(Viewer)); QVERIFY(IsChild(reinterpret_cast<HWND>(Parent.winId()), Viewer));
        QTest::qWait(400); Activate(Parent);
        QTRY_COMPARE(GetForegroundWindow(), reinterpret_cast<HWND>(Parent.winId()));
        auto Focus = Worker([&] { return Host->ViewerCall(WireKind::ViewerFocus, {}); }); QVERIFY(Focus.HasValue());
        const auto NativeFocused = [&] {
            GUITHREADINFO Info{sizeof(GUITHREADINFO)};
            return GetGUIThreadInfo(GetWindowThreadProcessId(Viewer, nullptr), &Info)
                && (Info.hwndFocus == Viewer || IsChild(Viewer, Info.hwndFocus));
        };
        QTRY_VERIFY_WITH_TIMEOUT(NativeFocused(), 3000);
        if ((MenuKey > 0 && MenuKey <= 255) || !SettingsButton.isEmpty()) {
            GUITHREADINFO Ui{sizeof(Ui)}; const auto Thread = GetWindowThreadProcessId(Viewer, nullptr);
            QVERIFY(GetGUIThreadInfo(Thread, &Ui));
            const auto MenuClosed = qScopeGuard([&] { PostMessageW(Viewer, WM_CANCELMODE, 0, 0); });
            if (!SettingsButton.isEmpty()) {
                struct ControlSearch { QString text_; HWND found_ = nullptr; } Search{SettingsButton};
                EnumChildWindows(Viewer, [](HWND Child, LPARAM Param) -> BOOL {
                    auto& Value = *reinterpret_cast<ControlSearch*>(Param);
                    if (IsWindowVisible(Child) && NativeText(Child).startsWith(Value.text_)) { Value.found_ = Child; return FALSE; }
                    return TRUE;
                }, reinterpret_cast<LPARAM>(&Search));
                if (Search.found_) {
                    qInfo() << "Native settings control" << NativeClass(Search.found_) << NativeText(Search.found_);
                    QVERIFY(ClickWindow(Search.found_));
                } else {
                    const auto Item = Worker([&] { return AccessibleControl(Viewer, SettingsButton); });
                    if (!Item) {
                        EnumChildWindows(Viewer, [](HWND Child, LPARAM) -> BOOL {
                            if (IsWindowVisible(Child)) qInfo() << "Visible native control" << NativeClass(Child) << NativeText(Child); return TRUE;
                        }, 0);
                        const auto Shot = qEnvironmentVariable("HC_WLX_SCREENSHOT");
                        if (!Shot.isEmpty()) Parent.screen()->grabWindow(Parent.winId()).save(Shot + ".locator.png");
                    }
                    QVERIFY2(Item.has_value(), "Requested settings native/accessibility item was not found"); QVERIFY(ClickRect(*Item));
                }
            } else QVERIFY(PostMessageW(Ui.hwndFocus, WM_KEYDOWN, WPARAM(MenuKey), 1));
            const auto MenuOpen = [&] { return GetGUIThreadInfo(Thread, &Ui) && (Ui.flags & GUI_INMENUMODE); };
            QTRY_VERIFY_WITH_TIMEOUT(MenuOpen(), 3000);
            OperationContext PendingContext; PendingContext.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            auto Pending = std::async(std::launch::async, [&] { return Host->Ping(PendingContext); });
            QTRY_COMPARE_WITH_TIMEOUT(Host->Snapshot().runtime_["workState"].toString(), QString("Queued"), 1000);
            QTest::qWait(3000);
            QVERIFY(Host->Snapshot().running_); QVERIFY(!Host->Snapshot().failure_); QVERIFY(MenuOpen());
            QVERIFY(Pending.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready);
            qInfo() << "Compatibility settings menu survived 3000 ms with 200 ms heartbeat; toolbar" << SettingsButton << Host->Snapshot().runtime_;
            const auto MenuScreenshot = qEnvironmentVariable("HC_WLX_SCREENSHOT");
            if (!MenuScreenshot.isEmpty()) QVERIFY(Parent.screen()->grabWindow(0).save(MenuScreenshot + ".menu.png"));
            QVERIFY(PostMessageW(Ui.hwndMenuOwner, WM_CANCELMODE, 0, 0));
            QTRY_VERIFY_WITH_TIMEOUT(!MenuOpen(), 2000);
            QVERIFY(Worker([&] { return Pending.get(); }).HasValue());
            QVERIFY(Worker([&] { return Host->Ping(); }).HasValue());
        }
        auto Resize = Worker([&] { return Host->ViewerCall(WireKind::ViewerResize, {{"width", 800}, {"height", 500}}); }); QVERIFY(Resize.HasValue());
        const auto Search = qEnvironmentVariable("HC_WLX_SEARCH");
        if (!Search.isEmpty())
        {
            QVERIFY(Probe.Value()["search"].toBool());
            const auto Found = Worker([&] { return Host->ViewerCall(WireKind::ViewerCommand, {{"command", 5}, {"parameter", 1}, {"text", Search}}); });
            QVERIFY2(Found.HasValue(), Found.HasValue() ? "" : Found.Failure().code_.c_str()); qInfo() << "Search result" << Found.Value();
        }
        QTest::qWait(300);
        const auto Screenshot = qEnvironmentVariable("HC_WLX_SCREENSHOT");
        if (!Screenshot.isEmpty()) QVERIFY(Parent.screen()->grabWindow(Parent.winId()).save(Screenshot));
        QVERIFY(Worker([&] { return Host->ViewerCall(WireKind::ViewerClose, {}); }).HasValue()); QVERIFY(!IsWindow(Viewer));
        Worker([&] { Host->Close(); }); QVERIFY(!Host->Snapshot().running_); QVERIFY(!Host->Snapshot().forcedTermination_);
    }
    void sqliteProductRegression_data()
    {
        QTest::addColumn<int>("Iteration"); QTest::addColumn<bool>("Hang");
        const QStringList Modes{"ascii", "unicode", "lease"};
        for (int Index = 0; Index < 30; ++Index)
            QTest::newRow(qPrintable(QString("%1-%2").arg(Index, 2, 10, QChar('0')).arg(Modes[Index % 3]))) << Index << false;
        QTest::newRow("30-ui-hang-recovery") << 30 << true;
    }
    void sqliteProductRegression()
    {
        QFETCH(int, Iteration); QFETCH(bool, Hang);
        const auto InputDirectory = qEnvironmentVariable("HC_SQLITE_INPUT_DIRECTORY");
        if (InputDirectory.isEmpty()) QSKIP("Explicit owned SQLite snapshot/package inputs required");
        const QDir Input(InputDirectory); const auto Binary = Input.filePath("SQLiteViewer.wlx64"), Database = Input.filePath("esbonio.db");
        const auto Json = [](const QString& Path) { QFile File(Path); return File.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(File.readAll()).object() : QJsonObject{}; };
        const auto Owner = Json(Input.filePath("owner.json")), Plan = Json(Input.filePath("test-plan.json"));
        QCOMPARE(Owner["owner"].toString(), QString("viewer-ui-ipc-implementation-20261111"));
        QVERIFY(QDateTime::fromString(Owner["expiresUtc"].toString(), Qt::ISODateWithMs) > QDateTime::currentDateTimeUtc());
        const auto ExpectedHash = Plan["dbSha256"].toString().toLatin1(); QCOMPARE(ExpectedHash.size(), 64);
        QCOMPARE(FileHash(Database), ExpectedHash); QCOMPARE(FileHash(Database), Owner["databaseSha256"].toString().toLatin1().toLower());
        const QByteArray DllHash("c91b9798f98a34c3201b36eb9ffe1c8789398aa7acd9037d23b6d685510af0f8");
        QCOMPARE(FileHash(Binary), DllHash);
        for (const auto* Suffix : {"-wal", "-shm", "-journal"}) QVERIFY(!QFileInfo::exists(Database + Suffix));
        const auto Table = Plan["table"].toString(), Search = Plan["search"].toString();
        const auto OtherTable = Plan["navigationTable"].toString(), OtherSearch = Plan["navigationSearch"].toString();
        QVERIFY(!Table.isEmpty() && !OtherTable.isEmpty() && OtherTable != Table && !Search.isEmpty() && !OtherSearch.isEmpty());
        const auto Pe = HostPeFlags(host_); QVERIFY(Pe.HasValue()); QVERIFY(!Pe.Value()["highEntropy"].toBool());
        QVERIFY(Pe.Value()["dynamicBase"].toBool() && Pe.Value()["nxCompat"].toBool() && Pe.Value()["largeAddressAware"].toBool());

        QTemporaryDir Root; QVERIFY(Root.isValid()); const auto Mode = Hang ? 2 : Iteration % 3;
        const auto Folder = Mode == 1 ? Root.filePath(QString::fromUtf8("한글 입력")) : Root.path(); QVERIFY(QDir().mkpath(Folder));
        const auto Copy = QDir(Folder).filePath(Mode == 1 ? QString::fromUtf8("데이터베이스.db") : QString("snapshot.db"));
        QVERIFY(QFile::copy(Database, Copy)); QCOMPARE(FileHash(Copy), ExpectedHash);
        std::shared_ptr<IMaterializer> Materializer; std::shared_ptr<IMaterializedFileLease> Lease; auto Path = Copy;
        if (Mode == 2) {
            auto Made = Worker([&] { return MakeNativeMaterializer(Root.path().toStdString()); }); QVERIFY(Made.HasValue()); Materializer = std::move(Made.Value());
            auto Provider = MakeNativeReadProvider(LocatorKind::Local);
            auto Entry = Worker([&] { return Provider->Resolve({LocatorKind::Local, Copy.toUtf8().toStdString()}, {}).get(); }); QVERIFY(Entry.HasValue());
            auto Source = std::make_shared<ProviderContentSource>(Provider, Entry.Value());
            auto Materialized = Worker([&] { return MaterializePrivateContent(*Materializer, {Source, {}, {}, {{"-wal", 0}, {"-shm", 32768}}}, {}).get(); }); QVERIFY(Materialized.HasValue());
            Lease = std::move(Materialized.Value()); Path = QString::fromUtf8(Lease->LocalPath()); QCOMPARE(Leases(Root.path()).size(), 1);
        }
        auto Draft = Worker([&] { return InspectPluginBinary(Binary, PluginFormat::Wlx); }); QVERIFY(Draft.HasValue());
        auto Saved = Worker([&] { return SavePluginManifest(Draft.Value(), Root.path()); }); QVERIFY(Saved.HasValue());
        auto Inspected = Worker([&] { return InspectManifestFile(Saved.Value()); }); QVERIFY(Inspected.HasValue());
        const PluginApproval Approval{Inspected.Value().manifest_, true, false, Inspected.Value().manifest_.permissions_};
        QWidget Parent; Parent.resize(1100, 720); Parent.show(); QVERIFY(QTest::qWaitForWindowExposed(&Parent)); Activate(Parent);
        QElapsedTimer Timer; Timer.start();
        auto Started = Worker([&] { return PluginProcess::Start(host_, Saved.Value(), Approval, {}, {}, Root.filePath("lsplugin.ini")); });
        QVERIFY2(Started.HasValue(), Started.HasValue() ? "" : Started.Failure().code_.c_str()); auto Host = std::move(Started.Value());
        const auto Cleanup = qScopeGuard([&] { Worker([&] { Host.reset(); }); });
        const auto Pid = Host->Snapshot().processId_; WinHandle Peer(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, Pid)); QVERIFY(Peer.Valid());
        FILETIME Created{}, Exited{}, Kernel{}, User{}; QVERIFY(GetProcessTimes(Peer.Get(), &Created, &Exited, &Kernel, &User));
        const auto Identity = QString::number(Pid) + ':' + QString::number((qulonglong(Created.dwHighDateTime) << 32) | Created.dwLowDateTime);
        QVERIFY(!sqliteInstances_.contains(Identity)); sqliteInstances_.insert(Identity);
        PROCESS_MITIGATION_ASLR_POLICY Aslr{}; QVERIFY(GetProcessMitigationPolicy(Peer.Get(), ProcessASLRPolicy, &Aslr, sizeof(Aslr))); QVERIFY(!Aslr.EnableHighEntropy);
        PROCESS_MITIGATION_DEP_POLICY Dep{}; SetLastError(0); const bool DepKnown = GetProcessMitigationPolicy(Peer.Get(), ProcessDEPPolicy, &Dep, sizeof(Dep)); const auto DepError = GetLastError();
        if (DepKnown) QVERIFY(Dep.Enable);
        const auto Probe = Worker([&] { return Host->Probe(); }); QVERIFY(Probe.HasValue()); QVERIFY(Probe.Value()["unicode"].toBool() && Probe.Value()["search"].toBool());
        const QJsonObject Open{{"parent", QString::number(Parent.winId(), 16)}, {"file", Path}, {"width", qRound(1100 * Parent.devicePixelRatioF())}, {"height", qRound(720 * Parent.devicePixelRatioF())}, {"flags", 0}};
        OperationContext OpenContext; OpenContext.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        auto Opened = Worker([&] { return Host->ViewerCall(WireKind::ViewerOpen, Open, OpenContext); });
        QVERIFY2(Opened.HasValue(), Opened.HasValue() ? "" : Opened.Failure().code_.c_str()); const auto OpenMs = Timer.elapsed();
        const auto Viewer = Window(Opened.Value(), "viewer"); QVERIFY(IsWindow(Viewer));
        HWND Combo = nullptr, Grid = nullptr; QTRY_VERIFY_WITH_TIMEOUT((Combo = TableCombo(Viewer, Table)) != nullptr, 2000);
        QTRY_VERIFY_WITH_TIMEOUT((Grid = VisibleGrid(Viewer)) != nullptr, 2000);
        const auto Count = NativeMessage(Combo, CB_GETCOUNT); QVERIFY(Count.HasValue()); QVERIFY(Count.Value() >= 2);
        const auto Evidence = qEnvironmentVariable("HC_SQLITE_EVIDENCE_DIRECTORY"); QVERIFY(!Evidence.isEmpty()); QVERIFY(QDir().mkpath(Evidence));
        const auto Capture = [&](const QString& Stage) {
            const auto Shot = Parent.screen()->grabWindow(Parent.winId());
            return !Shot.isNull() && Shot.save(QDir(Evidence).filePath(QString("sqlite-%1-%2.png").arg(Iteration, 2, 10, QChar('0')).arg(Stage)));
        };
        const auto CheckTable = [&](const QString& Name, const QString& Text) -> Result<Unit> {
            auto Changed = Worker([&] { return SelectTable(Combo, Name); }); if (!Changed.HasValue()) return Changed.Failure();
            OperationContext Calls; Calls.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            auto Found = Worker([&] { return Host->ViewerCall(WireKind::ViewerCommand, {{"command", 5}, {"parameter", 1}, {"text", Text}}, Calls); });
            if (!Found.HasValue()) return Found.Failure();
            if (Found.Value()["result"].toInt(-1) != 0) return HostError("test-sqlite-search-not-found");
            QTest::qWait(100); return Unit{};
        };
        auto Checked = CheckTable(Table, Search); QVERIFY2(Checked.HasValue(), Checked.HasValue() ? "" : Checked.Failure().code_.c_str()); QVERIFY(Capture("primary"));
        Checked = CheckTable(OtherTable, OtherSearch); QVERIFY2(Checked.HasValue(), Checked.HasValue() ? "" : Checked.Failure().code_.c_str()); QVERIFY(Capture("navigation"));
        Checked = CheckTable(Table, Search); QVERIFY2(Checked.HasValue(), Checked.HasValue() ? "" : Checked.Failure().code_.c_str());
        QVERIFY(IsWindowVisible(Grid)); QVERIFY(Host->Snapshot().running_); QVERIFY(!Host->Snapshot().failure_);
        qInfo() << "SQLite product instance" << Iteration << "mode" << Mode << "pid/creation" << Identity << "open ms" << OpenMs
            << "tables" << Count.Value() << Table << OtherTable << "ASLR flags" << Aslr.Flags << "DEP known/error/flags" << DepKnown << DepError << Dep.Flags << Pe.Value();
        QElapsedTimer Close; Close.start();
        if (Hang) {
            const auto Before = Host->Snapshot().runtime_["probeSerial"].toString().toULongLong();
            WinHandle Thread(OpenThread(THREAD_SUSPEND_RESUME, FALSE, GetWindowThreadProcessId(Viewer, nullptr))); QVERIFY(Thread.Valid());
            QVERIFY(SuspendThread(Thread.Get()) != DWORD(-1)); const auto Resume = qScopeGuard([&] { ResumeThread(Thread.Get()); });
            QTRY_VERIFY_WITH_TIMEOUT(!Host->Snapshot().running_, 4000); const auto State = Host->Snapshot();
            QVERIFY(State.failure_); QCOMPARE(State.failure_->code_, std::string("host-ui-unresponsive")); QVERIFY(State.forcedTermination_);
            QVERIFY(State.runtime_["probeSerial"].toString().toULongLong() > Before); QVERIFY(State.runtime_["probeFailures"].toInt() >= 2);
            qInfo() << "SQLite actual UI thread hang reaped ms" << Close.elapsed() << State.runtime_;
        } else {
            OperationContext CloseContext; CloseContext.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            auto Closed = Worker([&] { return Host->ViewerCall(WireKind::ViewerClose, {}, CloseContext); }); QVERIFY(Closed.HasValue()); QVERIFY(!IsWindow(Viewer));
        }
        Worker([&] { Host->Close(); }); const auto Final = Host->Snapshot(); QVERIFY(!Final.running_); QCOMPARE(Final.activeProcesses_, 0u);
        if (!Hang) { QVERIFY(!Final.forcedTermination_); QCOMPARE(Final.exitCode_.value_or(1), 0u); }
        bool UnexpectedSidecar = false;
        for (const auto* Suffix : {"-wal", "-shm", "-journal"}) {
            QFile File(Path + Suffix); if (!File.exists()) continue;
            QVERIFY(File.open(QIODevice::ReadOnly)); const auto Header = File.read(64);
            qInfo() << "SQLite retired sidecar" << Suffix << "bytes" << File.size() << "header" << Header.toHex() << "sha256" << FileHash(File.fileName());
            const bool Cache = (QString(Suffix) == "-wal" && File.size() == 0) || (QString(Suffix) == "-shm" && File.size() <= 32768);
            if (Cache) continue; UnexpectedSidecar = true;
            // Preserve only an owned snapshot's diagnostic data before its
            // QTemporaryDir disappears. Never touch the original user's DB.
            const auto Prefix = QDir(Evidence).filePath(QString("sqlite-%1").arg(Iteration, 2, 10, QChar('0')));
            if (File.size() <= 1024 * 1024) QVERIFY(QFile::copy(File.fileName(), Prefix + Suffix));
            else { QFile Copy(Prefix + Suffix + ".header"); QVERIFY(Copy.open(QIODevice::WriteOnly)); QCOMPARE(Copy.write(Header), Header.size()); }
        }
        QCOMPARE(FileHash(Path), ExpectedHash); QVERIFY2(!UnexpectedSidecar, "Unexpected SQLite sidecar preserved in diagnostic evidence");
        if (Materializer) {
            const auto Cleaned = Worker([&] { Lease.reset(); return Materializer->DrainCleanup(); }); QVERIFY(Cleaned.errors_.empty()); QCOMPARE(Cleaned.busy_, std::size_t(0));
            QVERIFY(Leases(Root.path()).isEmpty()); Worker([&] { Materializer.reset(); });
        }
        QCOMPARE(FileHash(Database), ExpectedHash); QCOMPARE(FileHash(Binary), DllHash);
        Worker([&] { Host.reset(); });
        QVERIFY(Root.remove()); QVERIFY(!QFileInfo::exists(Path));
        for (const auto* Suffix : {"-wal", "-shm", "-journal"}) { QVERIFY(!QFileInfo::exists(Path + Suffix)); QVERIFY(!QFileInfo::exists(Database + Suffix)); }
        qInfo() << "SQLite close/cleanup ms" << Close.elapsed() << "lease/sidecars clean";
    }
    void sqliteHostedSessionLease_data()
    { QTest::addColumn<bool>("Hang"); QTest::newRow("normal") << false; QTest::newRow("actual-ui-hang") << true; }
    void sqliteHostedSessionLease()
    {
        QFETCH(bool, Hang); const auto InputPath = qEnvironmentVariable("HC_SQLITE_INPUT_DIRECTORY"); if (InputPath.isEmpty()) QSKIP("Explicit owned SQLite inputs required");
        const QDir Input(InputPath); QFile OwnerFile(Input.filePath("owner.json")); QVERIFY(OwnerFile.open(QIODevice::ReadOnly)); const auto Owner = QJsonDocument::fromJson(OwnerFile.readAll()).object(); OwnerFile.close();
        QCOMPARE(Owner["owner"].toString(), QString("viewer-ui-ipc-implementation-20261111")); QVERIFY(QDateTime::fromString(Owner["expiresUtc"].toString(), Qt::ISODateWithMs) > QDateTime::currentDateTimeUtc());
        const auto Database = Input.filePath("esbonio.db"), Binary = Input.filePath("SQLiteViewer.wlx64");
        const auto ExpectedHash = Owner["databaseSha256"].toString().toLatin1().toLower(); QCOMPARE(FileHash(Database), ExpectedHash);
        const QByteArray DllHash("c91b9798f98a34c3201b36eb9ffe1c8789398aa7acd9037d23b6d685510af0f8"); QCOMPARE(FileHash(Binary), DllHash);
        QFile Data(Database); QVERIFY(Data.open(QIODevice::ReadOnly)); const auto Bytes = Data.readAll(); Data.close();
        QFile PlanFile(Input.filePath("test-plan.json")); QVERIFY(PlanFile.open(QIODevice::ReadOnly)); const auto Plan = QJsonDocument::fromJson(PlanFile.readAll()).object(); PlanFile.close();
        QTemporaryDir Root; QVERIFY(Root.isValid()); auto Made = Worker([&] { return MakeNativeMaterializer(Root.path().toStdString()); }); QVERIFY(Made.HasValue()); auto Materializer = std::move(Made.Value());
        auto Draft = Worker([&] { return InspectPluginBinary(Binary, PluginFormat::Wlx); }); QVERIFY(Draft.HasValue());
        auto Saved = Worker([&] { return SavePluginManifest(Draft.Value(), Root.path()); }); QVERIFY(Saved.HasValue());
        auto Inspected = Worker([&] { return InspectManifestFile(Saved.Value()); }); QVERIFY(Inspected.HasValue());
        PluginViewerOptions Options; Options.executable_ = host_; Options.manifestPath_ = Saved.Value(); Options.binaryPath_ = Binary; Options.defaultIniPath_ = Root.filePath("lsplugin.ini");
        Options.approval_ = PluginApproval{Inspected.Value().manifest_, true, false, Inspected.Value().manifest_.permissions_};
        QWidget Parent; Parent.resize(1000, 700); Parent.show(); QVERIFY(QTest::qWaitForWindowExposed(&Parent)); Activate(Parent);
        PluginViewerService Service([&]() -> Result<std::shared_ptr<IMaterializer>> { return Materializer; });
        const auto Drain = qScopeGuard([&] { Service.CancelAll(); while (!Service.IsIdle()) QTest::qWait(10); });
        auto Opened = Service.Open(Options, MemoryContent(Bytes, "snapshot.db"), QString::number(Parent.winId(), 16), 1000, 700); QVERIFY(Opened.HasValue()); auto Session = Opened.Value();
        QTRY_VERIFY_WITH_TIMEOUT(Session->Snapshot().status_ == hc::viewer::ViewerStatus::Ready || Session->Details().idle_, 10000);
        const auto Opening = Session->Snapshot(); QVERIFY2(Opening.status_ == hc::viewer::ViewerStatus::Ready, Opening.error_ ? Opening.error_->code_.c_str() : "SQLite session failed");
        QCOMPARE(Leases(Root.path()).size(), 1); const auto Directory = QDir(Root.path() + "/hc-materialized-v1").filePath(Leases(Root.path()).front()); const auto Path = Directory + "/content.db";
        QCOMPARE(FileHash(Path), ExpectedHash);
        const auto Viewer = reinterpret_cast<HWND>(quintptr(Session->Details().child_.toULongLong(nullptr, 16))); QVERIFY(IsWindow(Viewer)); QVERIFY(VisibleGrid(Viewer));
        const auto Table = Plan["table"].toString(); const auto Combo = TableCombo(Viewer, Table); QVERIFY(Combo);
        QVERIFY(Worker([&] { return SelectTable(Combo, Table); }).HasValue()); const auto Search = Session->Search(Plan["search"].toString(), 1); QVERIFY(Search.HasValue());
        QTRY_VERIFY_WITH_TIMEOUT(Session->Details().commandSequence_ >= Search.Value(), 3000); QVERIFY(!Session->Details().commandError_);
        QElapsedTimer Timer; Timer.start();
        if (Hang) {
            WinHandle Thread(OpenThread(THREAD_SUSPEND_RESUME, FALSE, GetWindowThreadProcessId(Viewer, nullptr))); QVERIFY(Thread.Valid()); QVERIFY(SuspendThread(Thread.Get()) != DWORD(-1));
            const auto Resume = qScopeGuard([&] { ResumeThread(Thread.Get()); }); QTRY_VERIFY_WITH_TIMEOUT(Service.IsIdle(), 5000);
            QVERIFY(Session->Details().host_.forcedTermination_); QVERIFY(Session->Details().host_.failure_); QCOMPARE(Session->Details().host_.failure_->code_, std::string("host-ui-unresponsive"));
        } else { Session->Close(); QTRY_VERIFY_WITH_TIMEOUT(Service.IsIdle(), 4000); QVERIFY(!Session->Details().host_.forcedTermination_); QCOMPARE(Session->Details().host_.exitCode_.value_or(1), 0u); }
        QCOMPARE(Session->Details().host_.activeProcesses_, 0u); const auto Cleaned = Worker([&] { return Materializer->DrainCleanup(); }); QVERIFY(Cleaned.errors_.empty());
        QVERIFY(Leases(Root.path()).isEmpty()); QVERIFY(!QFileInfo::exists(Path)); QVERIFY(!QFileInfo::exists(Path + "-wal")); QVERIFY(!QFileInfo::exists(Path + "-shm"));
        QCOMPARE(FileHash(Database), ExpectedHash); QCOMPARE(FileHash(Binary), DllHash); qInfo() << "SQLite HostedViewerSession automatic auxiliary retirement" << "hang" << Hang << "ms" << Timer.elapsed();
    }
    void sdkSearch()
    {
        QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, Approval, true));
        QFile Ini(Directory.filePath("lsplugin.ini")); QVERIFY(Ini.open(QIODevice::WriteOnly)); Ini.write("[fixture]\r\nvalue=123\r\n"); Ini.close();
        QWidget Parent; Parent.resize(400, 250); Parent.show(); QVERIFY(QTest::qWaitForWindowExposed(&Parent));
        auto Started = Worker([&] { return PluginProcess::Start(host_, Directory.filePath("plugin.json"), Approval, {}, {}, Ini.fileName()); });
        QVERIFY(Started.HasValue()); auto Host = std::move(Started.Value()); const auto Cleanup = qScopeGuard([&] { Worker([&] { Host.reset(); }); });
        const auto Request = request(Parent, Directory.filePath("search.txt"));
        auto Opened = Worker([&] { return Host->ViewerCall(WireKind::ViewerOpen, Request); }); QVERIFY(Opened.HasValue());
        const auto Found = Worker([&] { return Host->ViewerCall(WireKind::ViewerCommand, {{"command", 5}, {"parameter", 1}, {"text", "한글"}}); }); QVERIFY(Found.HasValue());
        QVERIFY(Title(Window(Opened.Value(), "viewer")).contains("한글"));
    }
    void automaticProductSelection_data()
    {
        QTest::addColumn<QString>("Mode");
        for (const auto Mode : {"match", "no-match", "no-candidates", "denied", "image", "force-image", "multimedia", "cancel", "replace"})
            QTest::newRow(Mode) << QString::fromLatin1(Mode);
    }
    void automaticProductSelection()
    {
        QFETCH(QString, Mode);
        QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, Approval, true));
        auto Options = options(Directory, Approval); Options.defaultIniPath_ = Directory.filePath("lsplugin.ini");
        const QByteArray Expression = Mode == "no-match" ? "0" : Mode == "multimedia" ? "MULTIMEDIA" : "1";
        { QFile Ini(Options.defaultIniPath_); QVERIFY(Ini.open(QIODevice::WriteOnly));
            QVERIFY(Ini.write("[fixture]\r\nvalue=123\r\nexpression=" + Expression + "\r\n") > 0); }
        if (Mode == "denied") Options.approval_.approved_ = false;
        QWidget Main; Main.resize(640, 360); Main.show();
        using Selection = QtViewerSurface::PluginViewerSelectionMode;
        auto SelectedMode = Mode == "force-image" ? Selection::Force : Selection::Automatic;
        QtViewerHost Host(&Main, [&] { return MakeNativeMaterializer(Directory.path().toStdString()); }, [&] {
            return Mode == "no-candidates" ? std::vector<QtViewerSurface::PluginViewerChoice>{}
                : std::vector<QtViewerSurface::PluginViewerChoice>{{"sdk", "SDK fixture", Options}};
        }, [&] { return SelectedMode; });
        const auto Drain = qScopeGuard([&] { Host.CloseAll(); while (!Host.IsIdle()) QTest::qWait(10); });
        auto* Surface = Host.CreateSurface(&Main); Surface->resize(Main.size()); Surface->show();
        QSignalSpy Failed(Surface, &QtViewerSurface::Failed), Ready(Surface, &QtViewerSurface::Ready);
        const bool Image = Mode.contains("image") || Mode == "multimedia";
        QByteArray Bytes = "automatic content";
        if (Image)
        {
            QImage Picture(8, 8, QImage::Format_RGB32); Picture.fill(Qt::green);
            Bytes.clear(); QBuffer Buffer(&Bytes); QVERIFY(Buffer.open(QIODevice::WriteOnly)); QVERIFY(Picture.save(&Buffer, "PNG"));
        }
        if (Mode == "cancel" || Mode == "replace")
        {
            auto Slow = std::make_shared<WaitingContent>(); Surface->Open(Slow, hc::viewer::ViewerPurpose::FullViewer);
            QTRY_VERIFY_WITH_TIMEOUT(Slow->reading_.load(), 4000);
            if (Mode == "cancel")
            {
                Surface->Execute("cancel"); QTRY_VERIFY_WITH_TIMEOUT(Host.IsIdle(), 4000);
                QCOMPARE(Ready.size(), 0); QCOMPARE(Failed.size(), 0); QVERIFY(!Surface->IsReady());
                QVERIFY(Leases(Directory.path()).isEmpty()); return;
            }
        }
        // Use a misleading suffix to verify multimedia selection probes bytes.
        Surface->Open(MemoryContent(Bytes, "latest.txt"), hc::viewer::ViewerPurpose::FullViewer);
        if (Mode == "denied")
        {
            QTRY_COMPARE_WITH_TIMEOUT(Failed.size(), 1, 5000); QCOMPARE(Ready.size(), 0);
            QTRY_VERIFY_WITH_TIMEOUT(Host.IsIdle(), 4000); QVERIFY(Leases(Directory.path()).isEmpty()); return;
        }
        QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(), 5000); QCOMPARE(Ready.size(), 1); QCOMPARE(Failed.size(), 0);
        const bool Builtin = Mode == "no-match" || Mode == "no-candidates" || Mode == "image";
        QCOMPARE(Surface->findChild<QtPluginViewerSurface*>()->IsReady(), !Builtin);
        QCOMPARE(Surface->Snapshot().source_.name_, std::string("latest.txt"));
        if (Mode == "image") QCOMPARE(Surface->Snapshot().provider_, std::string("builtin.image"));
        // Read the changed policy on the next open of an existing surface.
        SelectedMode = Selection::Manual; Surface->Open(MemoryContent("builtin again"));
        QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(), 5000);
        QCOMPARE(Surface->Snapshot().provider_, std::string("builtin.text"));
        if (Mode == "match")
        {
            auto* Menu = Surface->findChild<QMenu*>("hc.viewer.plugins"); QVERIFY(Menu);
            QVERIFY(QMetaObject::invokeMethod(Menu, "aboutToShow", Qt::DirectConnection));
            Options.approval_.approved_ = false;
            auto* Automatic = Menu->findChild<QAction*>("hc.viewer.plugin.automatic"); QVERIFY(Automatic); Automatic->trigger();
            QTRY_COMPARE_WITH_TIMEOUT(Failed.size(), 1, 5000); QVERIFY(!Surface->IsReady());
            Surface->findChild<QAction*>("hc.viewer.fallback")->trigger();
            QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(), 5000);
            QCOMPARE(Surface->Snapshot().provider_, std::string("builtin.text"));
        }
        Surface->Close(); QTRY_VERIFY_WITH_TIMEOUT(Host.IsIdle(), 4000); QVERIFY(Leases(Directory.path()).isEmpty());
    }
    void sdkInitializationAndDetection_data()
    {
        QTest::addColumn<QString>("Mode");
        for (const auto* Mode : {"normal", "empty", "maximum", "ansi", "unterminated", "hang-defaults", "crash-defaults", "hang-detect", "crash-detect"})
            QTest::newRow(Mode) << QString::fromLatin1(Mode);
    }
    void sdkInitializationAndDetection()
    {
        QFETCH(QString, Mode); QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, Approval, true));
        QWidget Parent;
        const auto Ini = Directory.filePath("settings/lsplugin.ini"); QVERIFY(QDir().mkpath(QFileInfo(Ini).absolutePath()));
        { QFile File(Ini); QVERIFY(File.open(QIODevice::WriteOnly)); QVERIFY(File.write("[fixture]\r\nvalue=123\r\nmode=" + Mode.toLatin1() + "\r\n") > 0); }
        HostLimits Limits; Limits.startupMs_ = 1000; Limits.requestMs_ = 500; Limits.heartbeatMs_ = 1000;
        QElapsedTimer Clock; Clock.start(); qint64 Last = 0, Gap = 0; QTimer Beat; Beat.setInterval(10);
        connect(&Beat, &QTimer::timeout, this, [&] { const auto Now = Clock.elapsed(); Gap = std::max(Gap, Now - Last); Last = Now; }); Beat.start();
        auto Started = Worker([&] { return PluginProcess::Start(host_, Directory.filePath("plugin.json"), Approval, Limits, {}, Ini); });
        if (Mode.endsWith("defaults"))
        {
            QVERIFY(!Started.HasValue()); QVERIFY(Clock.elapsed() < 3000); QVERIFY(Gap < 200);
            // The package lock must be released after a failed startup, too.
            QFile Binary(Directory.filePath("test.wlx64")); QVERIFY(Binary.open(QIODevice::ReadWrite)); return;
        }
        QVERIFY2(Started.HasValue(), Started.HasValue() ? "" : Started.Failure().code_.c_str());
        auto Host = std::move(Started.Value()); const auto Cleanup = qScopeGuard([&] { Worker([&] { Host.reset(); }); });
        auto Probe = Worker([&] { return Host->Probe(); });
        if (Mode.endsWith("detect"))
        {
            QVERIFY(!Probe.HasValue()); Worker([&] { Host->Close(); }); QVERIFY(!Host->Snapshot().running_);
            QVERIFY(Clock.elapsed() < 3000); QVERIFY(Gap < 200); return;
        }
        if (Mode == "unterminated")
        { QVERIFY(!Probe.HasValue()); QCOMPARE(Probe.Failure().code_, std::string("host-wlx-detect-unterminated")); QVERIFY(Worker([&] { return Host->Ping(); }).HasValue()); return; }
        QVERIFY(Probe.HasValue()); QVERIFY(Probe.Value()["detectAvailable"].toBool());
        const auto Bytes = QByteArray::fromBase64(Probe.Value()["detectStringBytes"].toString().toLatin1());
        if (Mode == "normal") QCOMPARE(Bytes, QByteArray("EXT=\"TXT\""));
        else if (Mode == "maximum") QCOMPARE(Bytes, QByteArray(2047, 'X'));
        else if (Mode == "ansi") QCOMPARE(Bytes, QByteArray::fromHex("5b305d3d22e922"));
        else QVERIFY(Bytes.isEmpty());
        const auto Again = Worker([&] { return Host->Probe(); }); QVERIFY(Again.HasValue()); QCOMPARE(Again.Value(), Probe.Value());
        Parent.resize(320, 200); Parent.show(); QVERIFY(QTest::qWaitForWindowExposed(&Parent));
        const auto Request = request(Parent, Directory.filePath("content.txt"));
        auto Opened = Worker([&] { return Host->ViewerCall(WireKind::ViewerOpen, Request); });
        QVERIFY(Opened.HasValue()); QCOMPARE(Title(Window(Opened.Value(), "viewer")), QString("WLX SDK initialized"));
    }
    void sdkConfigurationPaths_data()
    {
        QTest::addColumn<QString>("Name"); QTest::addColumn<bool>("Valid");
        QTest::newRow("unicode") << QString::fromUtf8("설정_😀/lsplugin.ini") << true;
        QTest::newRow("overlong-filename") << QString(270, 'a') + ".ini" << false;
    }
    void sdkConfigurationPaths()
    {
        QFETCH(QString, Name); QFETCH(bool, Valid);
        QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, Approval, true));
        const auto Ini = Directory.filePath(Name);
        if (Valid)
        {
            QVERIFY(QDir().mkpath(QFileInfo(Ini).absolutePath()));
            QFile File(Ini); QVERIFY(File.open(QIODevice::WriteOnly)); QVERIFY(File.write("[fixture]\r\nvalue=123\r\n") > 0);
        }
        auto Started = Worker([&] { return PluginProcess::Start(host_, Directory.filePath("plugin.json"), Approval, {}, {}, Ini); });
        if (!Started.HasValue())
        {
            // A volume without short names may not represent this directory in
            // the active ANSI code page. It must fail explicitly, never redirect.
            QCOMPARE(Started.Failure().code_, std::string("host-wlx-config-encoding")); return;
        }
        QVERIFY(Valid); auto Host = std::move(Started.Value());
        const auto Cleanup = qScopeGuard([&] { Worker([&] { Host.reset(); }); });
        auto Probe = Worker([&] { return Host->Probe(); }); QVERIFY(Probe.HasValue());
        QCOMPARE(QByteArray::fromBase64(Probe.Value()["detectStringBytes"].toString().toLatin1()), QByteArray("EXT=\"TXT\""));
    }
    void sdkSessionConfiguration()
    {
        QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, Approval, true));
        auto Options = options(Directory, Approval); Options.defaultIniPath_ = Directory.filePath("lsplugin.ini");
        { QFile File(Options.defaultIniPath_); QVERIFY(File.open(QIODevice::WriteOnly)); QVERIFY(File.write("[fixture]\r\nvalue=123\r\n") > 0); }
        QWidget Main; Main.resize(320, 200); Main.show();
        PluginViewerService Service([&] { return MakeNativeMaterializer(Directory.path().toStdString()); });
        const auto Drain = qScopeGuard([&] { Service.CancelAll(); while (!Service.IsIdle()) QTest::qWait(10); });
        auto Opened = Service.Open(Options, MemoryContent("sdk"), QString::number(Main.winId(), 16), 320, 200); QVERIFY(Opened.HasValue());
        auto Session = Opened.Value(); QTRY_COMPARE_WITH_TIMEOUT(Session->Snapshot().status_, hc::viewer::ViewerStatus::Ready, 4000);
        Session->Close(); QTRY_VERIFY_WITH_TIMEOUT(Service.IsIdle(), 4000); QVERIFY(Leases(Directory.path()).isEmpty());
    }
    void viewerHostSelectionAndClose()
    {
        QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, Approval));
        QWidget Main; Main.resize(640, 360); Main.show();
        bool Approved = true;
        QtViewerHost Host(&Main, [&] { return MakeNativeMaterializer(Directory.path().toStdString()); }, [&] {
            auto Options = options(Directory, Approval); Options.approval_.approved_ = Approved;
            return std::vector<QtViewerSurface::PluginViewerChoice>{{"test", "WLX fixture", Options}};
        });
        const auto Drain = qScopeGuard([&] { Host.CloseAll(); while (!Host.IsIdle()) QTest::qWait(10); });
        QPointer<QDialog> Full = Host.OpenFull(MemoryContent("content", "host.txt"));
        auto* Surface = Full->findChild<QtViewerSurface*>(); QVERIFY(Surface);
        QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(), 5000);
        auto* Menu = Surface->findChild<QMenu*>("hc.viewer.plugins"); QVERIFY(Menu);
        QVERIFY(QMetaObject::invokeMethod(Menu, "aboutToShow", Qt::DirectConnection));
        auto* Choice = Menu->findChild<QAction*>("hc.viewer.plugin.test"); QVERIFY(Choice); Choice->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(), 5000);
        QVERIFY(Surface->findChild<QtPluginViewerSurface*>()->IsReady()); QCOMPARE(Surface->Snapshot().source_.name_, std::string("host.txt"));
        QVERIFY(!Surface->findChild<QAction*>("hc.viewer.find")->isEnabled());
        QPointer<QWidget> Anchor = Surface->findChild<QWidget*>("hc.wlx.native-parent"); QVERIFY(Anchor);
        Surface->findChild<QAction*>("hc.viewer.fallback")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(), 5000); QTRY_VERIFY_WITH_TIMEOUT(Anchor.isNull(), 4000);
        QCOMPARE(Surface->findChild<QPlainTextEdit*>()->toPlainText(), QString("content"));
        QSignalSpy Failed(Surface, &QtViewerSurface::Failed);
        // A menu built under approved settings must re-check a later revocation.
        Approved = false; Choice->trigger(); QTRY_COMPARE_WITH_TIMEOUT(Failed.size(), 1, 5000); QVERIFY(!Surface->IsReady());
        QTRY_VERIFY_WITH_TIMEOUT(Host.IsIdle(), 4000); QVERIFY(Leases(Directory.path()).isEmpty());
        Full->close(); QTRY_VERIFY(Full.isNull());
        Approved = true;
        Full = Host.OpenFull(MemoryContent("HC-WLX-FAULT:hang-close", "close.txt"));
        Surface = Full->findChild<QtViewerSurface*>(); QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(), 5000);
        Menu = Surface->findChild<QMenu*>("hc.viewer.plugins"); QVERIFY(QMetaObject::invokeMethod(Menu, "aboutToShow", Qt::DirectConnection));
        Menu->findChild<QAction*>("hc.viewer.plugin.test")->trigger(); QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(), 5000);
        QVERIFY(Surface->findChild<QtPluginViewerSurface*>()->IsReady());
        QElapsedTimer Closing; Closing.start(); Main.close(); QVERIFY(Closing.elapsed() < 200);
        QVERIFY(Main.isVisible()); QTRY_VERIFY_WITH_TIMEOUT(!Main.isVisible(), 4000); QVERIFY(Host.IsIdle());
        QVERIFY(Leases(Directory.path()).isEmpty());
    }
    void qtSurfaceReplacement()
    {
        QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, Approval));
        QWidget Main; Main.resize(640, 360); auto* Layout = new QVBoxLayout(&Main);
        QtPluginViewerController Controller(&Main, [&] { return MakeNativeMaterializer(Directory.path().toStdString()); });
        auto* Surface = new QtPluginViewerSurface(&Controller, &Main); Layout->addWidget(Surface); Main.show(); QVERIFY(QTest::qWaitForWindowExposed(&Main));
        // Native activation can be displaced by pending events from the previous
        // test window. Retry acquisition while pumping events, but still require
        // real foreground ownership before asserting the hosted child's focus.
        QTRY_COMPARE_WITH_TIMEOUT(([&] { Activate(Main); return GetForegroundWindow(); })(), reinterpret_cast<HWND>(Main.winId()), 5000);
        Surface->setFocus();
        const auto Drain = qScopeGuard([&] { Surface->Close(); while (!Controller.IsIdle()) QTest::qWait(10); });
        QSignalSpy Ready(Surface, &QtPluginViewerSurface::Ready); QSignalSpy Failed(Surface, &QtPluginViewerSurface::Failed);
        Surface->Open(options(Directory, Approval), MemoryContent("first", "first.txt"));
        QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(), 5000); QCOMPARE(Ready.size(), 1); QCOMPARE(Failed.size(), 0);
        const auto Focused = [&] {
            const auto Container = GetWindow(reinterpret_cast<HWND>(Surface->findChild<QWidget*>("hc.wlx.native-parent")->winId()), GW_CHILD);
            const auto Child = GetWindow(Container, GW_CHILD); GUITHREADINFO Info{sizeof(GUITHREADINFO)};
            return GetGUIThreadInfo(GetWindowThreadProcessId(Child, nullptr), &Info) && (Info.hwndFocus == Child || IsChild(Child, Info.hwndFocus));
        };
        QTRY_VERIFY_WITH_TIMEOUT(Focused(), 3000);
        QPointer<QWidget> Native = Surface->findChild<QWidget*>("hc.wlx.native-parent"); QVERIFY(Native); const auto Before = Native->winId();
        auto Slow = std::make_shared<WaitingContent>(); Surface->Open(options(Directory, Approval), Slow);
        if (Native) QCOMPARE(Native->winId(), Before);
        QTRY_VERIFY_WITH_TIMEOUT(Slow->reading_.load(), 3000);
        Surface->Open(options(Directory, Approval), MemoryContent("latest", "latest.txt"));
        QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(), 5000); QCOMPARE(Ready.size(), 2); QCOMPARE(Failed.size(), 0);
        QCOMPARE(Surface->Snapshot().source_.name_, std::string("latest.txt"));
        Main.resize(380, 260); QTest::qWait(100);
        const auto Screenshots = qEnvironmentVariable("HC_VIEWER_SCREENSHOTS");
        if (!Screenshots.isEmpty()) { QDir().mkpath(Screenshots); QVERIFY(Main.screen()->grabWindow(Main.winId()).save(Screenshots + "/wlx-surface.png")); }
        Surface->Close(); QTRY_VERIFY_WITH_TIMEOUT(Controller.IsIdle(), 4000); QVERIFY(Leases(Directory.path()).isEmpty());
    }
    void qtSurfaceDestruction_data()
    {
        QTest::addColumn<QString>("Name");
        for (const auto Name : {"ordinary", "hang-message", "hang-close"}) QTest::newRow(Name) << QString(Name);
    }
    void qtSurfaceDestruction()
    {
        QFETCH(QString, Name); QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, Approval));
        QWidget Main; Main.resize(640, 360); auto* Layout = new QVBoxLayout(&Main);
        QtPluginViewerController Controller(&Main, [&] { return MakeNativeMaterializer(Directory.path().toStdString()); });
        auto* Surface = new QtPluginViewerSurface(&Controller, &Main); Layout->addWidget(Surface); Main.show(); QVERIFY(QTest::qWaitForWindowExposed(&Main));
        const auto Drain = qScopeGuard([&] { Controller.CloseAll(); while (!Controller.IsIdle()) QTest::qWait(10); });
        Surface->Open(options(Directory, Approval), MemoryContent("HC-WLX-FAULT:" + Name.toUtf8()));
        QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(), 5000);
        QPointer<QWidget> Native = Surface->findChild<QWidget*>("hc.wlx.native-parent"); QVERIFY(Native); const auto Before = Native->winId();
        if (Name == "hang-message") QTest::qWait(120);
        QElapsedTimer Deletion; Deletion.start(); delete Surface;
        qInfo() << "Qt surface deletion" << Name << "ms" << Deletion.elapsed(); QVERIFY(Deletion.elapsed() < 200);
        if (Native) { QCOMPARE(Native->winId(), Before); QVERIFY(!Native->isVisible()); }
        QTRY_VERIFY_WITH_TIMEOUT(Controller.IsIdle(), 4000); QTRY_VERIFY_WITH_TIMEOUT(Native.isNull(), 1000);
        QVERIFY(Leases(Directory.path()).isEmpty());
    }
    void sessionMaterializationAndCommands()
    {
        QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, Approval));
        QWidget Parent; Parent.resize(640, 360); Parent.show(); QVERIFY(QTest::qWaitForWindowExposed(&Parent));
        const auto GuiThread = std::this_thread::get_id(); std::atomic<bool> FactoryOnWorker = false;
        PluginViewerService Service([&] { FactoryOnWorker = std::this_thread::get_id() != GuiThread; return MakeNativeMaterializer(Directory.path().toStdString()); });
        const auto Drain = qScopeGuard([&] { Service.CancelAll(); while (!Service.IsIdle()) QTest::qWait(10); });
        auto Source = MemoryContent("materialized Unicode content"); const auto Expected = Source->Describe();
        QElapsedTimer Start; Start.start();
        auto Opened = Service.Open(options(Directory, Approval), Source, QString::number(Parent.winId(), 16), 640, 360);
        QVERIFY(Start.elapsed() < 100); QVERIFY(Opened.HasValue()); auto Session = Opened.Value();
        QTRY_VERIFY_WITH_TIMEOUT(Session->Snapshot().status_ != hc::viewer::ViewerStatus::Loading, 5000);
        QVERIFY2(Session->Snapshot().status_ == hc::viewer::ViewerStatus::Ready,
            Session->Snapshot().error_ ? Session->Snapshot().error_->code_.c_str() : "not ready");
        QVERIFY(FactoryOnWorker); QCOMPARE(Session->Snapshot().source_.id_, Expected.id_); QCOMPARE(Session->Snapshot().bytesRead_, Expected.size_);
        const auto LeaseNames = Leases(Directory.path()); QCOMPARE(LeaseNames.size(), 1);
        const auto ContentPath = Directory.path() + "/hc-materialized-v1/" + LeaseNames.front() + "/content.txt";
        QFile Content(ContentPath); QVERIFY(Content.open(QIODevice::ReadOnly)); QCOMPARE(Content.readAll(), QByteArray("materialized Unicode content")); Content.close();
        const auto Details = Session->Details(); const auto Child = reinterpret_cast<HWND>(static_cast<quintptr>(Details.child_.toULongLong(nullptr, 16)));
        for (unsigned Size = 100; Size <= 600; ++Size) QVERIFY(Session->Resize(Size, Size / 2).HasValue());
        QTRY_VERIFY_WITH_TIMEOUT(([&] { RECT Rect{}; return GetClientRect(Child, &Rect) && Rect.right == 600 && Rect.bottom == 300; })(), 3000);
        QVERIFY(Session->Focus().HasValue()); auto Command = Session->SendCommand(3, 0); QVERIFY(Command.HasValue());
        QTRY_COMPARE_WITH_TIMEOUT(Session->Details().commandSequence_, Command.Value(), 3000); QVERIFY(!Session->Details().commandError_);
        QElapsedTimer Closing; Closing.start(); Session->Close(); QVERIFY(Closing.elapsed() < 100);
        QTRY_VERIFY_WITH_TIMEOUT(Service.IsIdle(), 4000); QVERIFY(Session->Details().idle_); QVERIFY(!Session->Details().host_.running_);
        QVERIFY(!IsWindow(Child)); QVERIFY(Leases(Directory.path()).isEmpty()); QVERIFY(!QFileInfo::exists(ContentPath));
        QCOMPARE(Session->Snapshot().status_, hc::viewer::ViewerStatus::Closed); QVERIFY(!Session->Resize(1, 1).HasValue()); Service.Collect();
    }
    void sessionReadCancellationAndBudget()
    {
        QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, Approval)); QWidget Parent;
        PluginViewerService Service([&] { return MakeNativeMaterializer(Directory.path().toStdString()); }, 1);
        const auto Drain = qScopeGuard([&] { Service.CancelAll(); while (!Service.IsIdle()) QTest::qWait(10); });
        auto Source = std::make_shared<WaitingContent>();
        auto Opened = Service.Open(options(Directory, Approval), Source, QString::number(Parent.winId(), 16), 320, 200);
        QVERIFY(Opened.HasValue()); QTRY_VERIFY_WITH_TIMEOUT(Source->reading_.load(), 3000);
        auto Second = Service.Open(options(Directory, Approval), MemoryContent("second"), QString::number(Parent.winId(), 16), 320, 200);
        QVERIFY(!Second.HasValue()); QCOMPARE(Second.Failure().category_, ErrorCategory::BudgetExceeded);
        Opened.Value()->Cancel(); QTRY_VERIFY_WITH_TIMEOUT(Service.IsIdle(), 4000);
        QCOMPARE(Opened.Value()->Snapshot().status_, hc::viewer::ViewerStatus::Cancelled);
        QVERIFY(Leases(Directory.path()).isEmpty()); QCOMPARE(Opened.Value()->Details().host_.processId_, 0u);
        auto Third = Service.Open(options(Directory, Approval), MemoryContent("third"), QString::number(Parent.winId(), 16), 320, 200);
        QVERIFY(Third.HasValue()); QTRY_COMPARE_WITH_TIMEOUT(Third.Value()->Snapshot().status_, hc::viewer::ViewerStatus::Ready, 5000);
        Third.Value().reset(); QTRY_VERIFY_WITH_TIMEOUT(Service.IsIdle(), 4000); QVERIFY(Leases(Directory.path()).isEmpty());
    }
    void sessionAdmissionBeforeExtraction()
    {
        QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, Approval)); QWidget Parent; std::atomic<unsigned> Factories = 0;
        PluginViewerService Service([&] { ++Factories; return MakeNativeMaterializer(Directory.path().toStdString()); });
        const auto Drain = qScopeGuard([&] { Service.CancelAll(); while (!Service.IsIdle()) QTest::qWait(10); });
        auto Options = options(Directory, Approval); Options.binaryPath_ = Directory.filePath("wrong.wlx64");
        auto Opened = Service.Open(Options, MemoryContent("secret"), QString::number(Parent.winId(), 16), 320, 200);
        QVERIFY(Opened.HasValue()); QTRY_VERIFY_WITH_TIMEOUT(Service.IsIdle(), 4000);
        QCOMPARE(Opened.Value()->Snapshot().status_, hc::viewer::ViewerStatus::Failed);
        QCOMPARE(Opened.Value()->Snapshot().error_->code_, std::string("plugin-configured-binary-mismatch")); QCOMPARE(Factories.load(), 0u);
        Options = options(Directory, Approval); Options.approval_.granted_.erase(Permission::UserInterface);
        auto Denied = Service.Open(Options, MemoryContent("secret"), QString::number(Parent.winId(), 16), 320, 200);
        QVERIFY(Denied.HasValue()); QTRY_VERIFY_WITH_TIMEOUT(Service.IsIdle(), 4000);
        QCOMPARE(Denied.Value()->Snapshot().error_->code_, std::string("plugin-permission-denied")); QCOMPARE(Factories.load(), 0u);
    }
    void sessionFaultCleanup_data()
    {
        QTest::addColumn<QString>("Name");
        for (const auto Name : {"hang-message", "crash-command", "hang-close", "hang-load"}) QTest::newRow(Name) << QString(Name);
    }
    void sessionFaultCleanup()
    {
        QFETCH(QString, Name); QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, Approval));
        QWidget Parent; Parent.resize(320, 200); Parent.show(); QVERIFY(QTest::qWaitForWindowExposed(&Parent));
        PluginViewerService Service([&] { return MakeNativeMaterializer(Directory.path().toStdString()); });
        const auto Drain = qScopeGuard([&] { Service.CancelAll(); while (!Service.IsIdle()) QTest::qWait(10); });
        QElapsedTimer Clock; Clock.start(); qint64 Last = 0, Gap = 0; unsigned Ticks = 0;
        QTimer Beat; Beat.setInterval(10); connect(&Beat, &QTimer::timeout, this, [&] { const auto Now = Clock.elapsed(); Gap = std::max(Gap, Now - Last); Last = Now; ++Ticks; }); Beat.start();
        auto Opened = Service.Open(options(Directory, Approval), MemoryContent("HC-WLX-FAULT:" + Name.toUtf8()), QString::number(Parent.winId(), 16), 320, 200);
        QVERIFY(Opened.HasValue()); auto Session = Opened.Value();
        if (Name == "hang-load")
        { QTRY_VERIFY_WITH_TIMEOUT(Session->Details().host_.processId_ != 0, 3000); Session->Cancel(); }
        else
        {
            QTRY_COMPARE_WITH_TIMEOUT(Session->Snapshot().status_, hc::viewer::ViewerStatus::Ready, 3000);
            if (Name == "crash-command") QVERIFY(Session->SendCommand(3, 0).HasValue());
            if (Name == "hang-close") Session->Close();
        }
        QTRY_VERIFY_WITH_TIMEOUT(Service.IsIdle(), 5000); QVERIFY(Leases(Directory.path()).isEmpty()); QVERIFY(!Session->Details().host_.running_);
        const auto Expected = Name == "hang-load" ? hc::viewer::ViewerStatus::Cancelled : Name == "hang-close" ? hc::viewer::ViewerStatus::Closed : hc::viewer::ViewerStatus::Failed;
        QCOMPARE(Session->Snapshot().status_, Expected); QVERIFY(Ticks > 2);
        qInfo() << "Session GUI maximum gap ms" << Gap; QVERIFY(Gap < 200);
    }
    void modalSettings_data()
    {
        QTest::addColumn<bool>("Command");
        QTest::addColumn<QString>("Finish");
        QTest::addColumn<bool>("Menu");
        QTest::newRow("toolbar-resume") << false << QString("resume") << false;
        QTest::newRow("command-resume") << true << QString("resume") << false;
        QTest::newRow("toolbar-close") << false << QString("close") << false;
        QTest::newRow("command-cancel") << true << QString("cancel") << false;
        QTest::newRow("toolbar-hung-dialog") << false << QString("hang") << false;
        QTest::newRow("menu-resume") << false << QString("resume") << true;
        QTest::newRow("menu-large-frame") << false << QString("large") << true;
        QTest::newRow("menu-close") << false << QString("close") << true;
        QTest::newRow("menu-hung-owner") << false << QString("hang") << true;
    }
    void modalSettings()
    {
        QFETCH(bool, Command); QFETCH(QString, Finish); QFETCH(bool, Menu);
        QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, Approval));
        QWidget Parent; Parent.resize(320, 200); Parent.show(); QVERIFY(QTest::qWaitForWindowExposed(&Parent));
        auto Started = Worker([&] { return PluginProcess::Start(host_, Directory.filePath("plugin.json"), Approval, options(Directory, Approval).limits_); });
        QVERIFY(Started.HasValue()); auto Host = std::move(Started.Value());
        const auto Cleanup = qScopeGuard([&] { Worker([&] { Host.reset(); }); });
        const auto Request = request(Parent, Directory.filePath("settings.txt"));
        auto Opened = Worker([&] { return Host->ViewerCall(WireKind::ViewerOpen, Request); });
        QVERIFY(Opened.HasValue()); const auto Viewer = Window(Opened.Value(), "viewer");
        const auto Pid = Host->Snapshot().processId_;
        std::stop_source Stop; OperationContext Context; Context.stopToken_ = Stop.get_token();
        std::future<Result<QJsonObject>> Pending;
        if (Command) Pending = std::async(std::launch::async, [&] {
            return Host->ViewerCall(WireKind::ViewerCommand, {{"command", 4}, {"parameter", 78}}, Context);
        });
        else QVERIFY(PostMessageW(Viewer, WM_APP + (Menu ? 82 : 80), 0, 0));
        HWND Dialog = nullptr;
        const auto FindDialog = [&] {
            if (Menu) {
                GUITHREADINFO Ui{sizeof(Ui)};
                if (!GetGUIThreadInfo(GetWindowThreadProcessId(Viewer, nullptr), &Ui) || !(Ui.flags & GUI_INMENUMODE)) return false;
                Dialog = Ui.hwndMenuOwner; return Dialog != nullptr;
            }
            struct Search { DWORD pid_; HWND dialog_ = nullptr; } Found{Pid};
            EnumWindows([](HWND Window, LPARAM Context) -> BOOL {
                auto& Search = *reinterpret_cast<struct Search*>(Context);
                DWORD Pid = 0; GetWindowThreadProcessId(Window, &Pid);
                if (Pid == Search.pid_ && Title(Window) == "WLX fixture settings") { Search.dialog_ = Window; return FALSE; }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&Found));
            Dialog = Found.dialog_; return Dialog != nullptr;
        };
        // On an assertion failure dismiss the modal before joining the async call.
        const auto Dismiss = qScopeGuard([&] {
            Stop.request_stop(); if (Dialog) PostMessageW(Dialog, Menu ? WM_CANCELMODE : WM_CLOSE, 0, 0);
            if (Pending.valid()) Worker([&] { return Pending.get(); });
        });
        QTRY_VERIFY_WITH_TIMEOUT(FindDialog(), 2000);
        QTest::qWait(1600); // Several heartbeat and request budgets.
        QVERIFY(IsWindow(Dialog)); QVERIFY(Host->Snapshot().running_);
        QVERIFY(!Host->Snapshot().failure_);
        const auto DuringModal = Host->Snapshot().runtime_;
        QVERIFY(DuringModal["probeSerial"].toString().toULongLong() > 1);
        QVERIFY(DuringModal["uiAgeMs"].toInteger(1000) < 500);
        if (Command) QVERIFY(Pending.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready);
        const bool Resume = Finish == "resume" || Finish == "large";
        if (Menu && Resume) {
            RECT Before{}; QVERIFY(GetClientRect(Viewer, &Before));
            Pending = std::async(std::launch::async, [&] {
                QJsonObject Resize{{"width", 300}, {"height", 190}};
                // A valid frame with deliberately invalid command fields still
                // has to cross a full pipe while the UI is in TrackPopupMenu.
                if (Finish == "large") Resize["padding"] = QString(256 * 1024, 'x');
                return Host->ViewerCall(WireKind::ViewerResize, Resize, Context);
            });
            QTest::qWait(800);
            QVERIFY(Pending.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready);
            QCOMPARE(Host->Snapshot().runtime_["workState"].toString(), QString("Queued"));
            RECT During{}; QVERIFY(GetClientRect(Viewer, &During));
            QCOMPARE(During.right, Before.right); QCOMPARE(During.bottom, Before.bottom);
        }
        if (Resume) {
            QVERIFY(PostMessageW(Dialog, Menu ? WM_CANCELMODE : WM_CLOSE, 0, 0));
            QTRY_VERIFY_WITH_TIMEOUT(!FindDialog(), 2000);
            if (Pending.valid()) {
                const auto Reply = Worker([&] { return Pending.get(); });
                if (Finish == "large") { QVERIFY(!Reply.HasValue()); QCOMPARE(Reply.Failure().code_, std::string("host-viewer-size")); }
                else QVERIFY(Reply.HasValue());
            }
            QVERIFY(Worker([&] { return Host->Ping(); }).HasValue());
            QVERIFY(Worker([&] { return Host->ViewerCall(WireKind::ViewerResize, {{"width", 280}, {"height", 180}}); }).HasValue());
            QVERIFY(Worker([&] { return Host->ViewerCall(WireKind::ViewerClose, {}); }).HasValue());
            Worker([&] { Host->Close(); }); QVERIFY(!Host->Snapshot().forcedTermination_);
        } else if (Finish == "close") {
            QElapsedTimer Clock; Clock.start(); Worker([&] { Host->Close(); });
            QVERIFY(Clock.elapsed() < 3000); QVERIFY(!Host->Snapshot().running_);
        } else if (Finish == "cancel") {
            Stop.request_stop(); const auto Reply = Worker([&] { return Pending.get(); });
            QVERIFY(!Reply.HasValue()); QCOMPARE(Reply.Failure().category_, ErrorCategory::Cancelled);
            QCOMPARE(Reply.Failure().code_, std::string("host-caller-detached"));
            // WLX has no cooperative cancellation ABI. Detaching a subscription
            // preserves the dialog and SDK call; the reader retires its reply.
            QVERIFY(Host->Snapshot().running_); QVERIFY(Host->Snapshot().callerDetached_);
            QVERIFY(PostMessageW(Dialog, WM_CLOSE, 0, 0));
            OperationContext Next; Next.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            QVERIFY(Worker([&] { return Host->Ping(Next); }).HasValue());
            QVERIFY(!Host->Snapshot().callerDetached_);
        } else {
            QVERIFY(PostMessageW(Dialog, WM_APP + 81, 0, 0));
            QTRY_VERIFY_WITH_TIMEOUT(!Host->Snapshot().running_, 4000);
            QVERIFY(Host->Snapshot().failure_);
        }
    }
    void admission()
    {
        QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, Approval));
        Approval.granted_.erase(Permission::UserInterface);
        auto Started = Worker([&] { return PluginProcess::Start(host_, Directory.filePath("plugin.json"), Approval); });
        QVERIFY(!Started.HasValue()); QCOMPARE(Started.Failure().code_, std::string("plugin-permission-denied"));
    }
    void nativeLifecycle()
    {
        QTemporaryDir Directory(QDir::tempPath() + "/WLX 한글 XXXXXX"); PluginApproval Approval; QVERIFY(prepare(Directory, Approval));
        QWidget Parent; Parent.resize(640, 360); Parent.show(); QVERIFY(QTest::qWaitForWindowExposed(&Parent));
        Activate(Parent);
        QTRY_COMPARE(GetForegroundWindow(), reinterpret_cast<HWND>(Parent.winId()));
        const auto Expected = qEnvironmentVariable("HC_VIEWER_EXPECT_DPR");
        if (!Expected.isEmpty()) QCOMPARE(Parent.devicePixelRatioF(), Expected.toDouble());
        auto Started = Worker([&] { return PluginProcess::Start(host_, Directory.filePath("plugin.json"), Approval); });
        QVERIFY2(Started.HasValue(), Started.HasValue() ? "" : Started.Failure().code_.c_str());
        auto Host = std::move(Started.Value()); const auto Cleanup = qScopeGuard([&] { Worker([&] { Host.reset(); }); });
        auto Probe = Worker([&] { return Host->Probe(); }); QVERIFY(Probe.HasValue()); QCOMPARE(Probe.Value()["format"].toString(), QString("wlx"));
        QVERIFY(!Probe.Value()["detectAvailable"].toBool()); QVERIFY(Probe.Value()["detectStringBytes"].toString().isEmpty());
        const auto Request = request(Parent, Directory.filePath("문서.txt"));
        auto Opened = Worker([&] { return Host->ViewerCall(WireKind::ViewerOpen, Request); });
        QVERIFY2(Opened.HasValue(), Opened.HasValue() ? "" : qPrintable(QString::fromStdString(Opened.Failure().code_) + " exit=" + QString::number(Host->Snapshot().exitCode_.value_or(0))));
        const auto Container = Window(Opened.Value(), "window"), Viewer = Window(Opened.Value(), "viewer");
        QCOMPARE(GetParent(Container), reinterpret_cast<HWND>(Parent.winId())); QVERIFY(IsChild(Container, Viewer));
        DWORD Pid = 0; GetWindowThreadProcessId(Viewer, &Pid); QCOMPARE(Pid, Host->Snapshot().processId_); QVERIFY(Pid != GetCurrentProcessId());
        QTRY_VERIFY_WITH_TIMEOUT(Title(Viewer).startsWith("WLX fixture ready ") && Title(Viewer).section(' ', -1).toUInt() >= 3, 3000);
        RECT Rect{}; QVERIFY(GetClientRect(Viewer, &Rect)); QCOMPARE(Rect.right, Request["width"].toInt()); QCOMPARE(Rect.bottom, Request["height"].toInt());
        auto Resized = Worker([&] { return Host->ViewerCall(WireKind::ViewerResize, {{"width", 333}, {"height", 177}}); });
        QVERIFY(Resized.HasValue()); QVERIFY(GetClientRect(Viewer, &Rect)); QCOMPARE(Rect.right, 333L); QCOMPARE(Rect.bottom, 177L);
        Activate(Parent); Parent.setFocus();
        QTRY_COMPARE(GetForegroundWindow(), reinterpret_cast<HWND>(Parent.winId()));
        auto Focused = Worker([&] { return Host->ViewerCall(WireKind::ViewerFocus, {}); }); QVERIFY(Focused.HasValue()); QVERIFY(Focused.Value()["focused"].toBool());
        auto Command = Worker([&] { return Host->ViewerCall(WireKind::ViewerCommand, {{"command", 3}, {"parameter", 0}}); });
        QVERIFY(Command.HasValue()); QCOMPARE(Command.Value()["result"].toInt(-1), 0);
        auto Invalid = Worker([&] { return Host->ViewerCall(WireKind::ViewerResize, {{"width", -1}, {"height", 1}}); }); QVERIFY(!Invalid.HasValue());
        QVERIFY(Worker([&] { return Host->Ping(); }).HasValue());
        QVERIFY(Worker([&] { return Host->ViewerCall(WireKind::ViewerClose, {}); }).HasValue()); QVERIFY(!IsWindow(Viewer)); QVERIFY(!IsWindow(Container));
        auto Again = Worker([&] { return Host->ViewerCall(WireKind::ViewerOpen, Request); }); QVERIFY(Again.HasValue());
        Worker([&] { Host->Close(); }); QVERIFY(!IsWindow(Window(Again.Value(), "viewer")));
        QCOMPARE(Host->Snapshot().exitCode_.value(), 0u); QVERIFY(!Host->Snapshot().forcedTermination_);
    }
    void foreignParentDenied()
    {
        QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, Approval));
        auto Started = Worker([&] { return PluginProcess::Start(host_, Directory.filePath("plugin.json"), Approval); });
        QVERIFY(Started.HasValue()); auto Host = std::move(Started.Value());
        const auto Cleanup = qScopeGuard([&] { Worker([&] { Host.reset(); }); });
        auto Denied = Worker([&] { return Host->ViewerCall(WireKind::ViewerOpen,
            {{"parent", QString::number(reinterpret_cast<quintptr>(GetDesktopWindow()), 16)}}); });
        QVERIFY(!Denied.HasValue()); QCOMPARE(Denied.Failure().code_, std::string("host-viewer-parent"));
        QVERIFY(Worker([&] { return Host->Ping(); }).HasValue());
    }
    void cancelOpen()
    {
        QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, Approval));
        QWidget Parent; Parent.resize(320, 200); Parent.show(); QVERIFY(QTest::qWaitForWindowExposed(&Parent));
        auto Started = Worker([&] { return PluginProcess::Start(host_, Directory.filePath("plugin.json"), Approval); });
        QVERIFY(Started.HasValue()); auto Host = std::move(Started.Value());
        const auto Cleanup = qScopeGuard([&] { Worker([&] { Host.reset(); }); });
        const auto Request = request(Parent, Directory.filePath("hang-load.txt"));
        std::stop_source Stop; OperationContext Context; Context.stopToken_ = Stop.get_token();
        QTimer Canceller; Canceller.setSingleShot(true); connect(&Canceller, &QTimer::timeout, this, [&] { Stop.request_stop(); }); Canceller.start(100);
        QElapsedTimer Elapsed; Elapsed.start();
        auto Opened = Worker([&] { return Host->ViewerCall(WireKind::ViewerOpen, Request, Context); });
        QVERIFY(!Opened.HasValue()); QCOMPARE(Opened.Failure().category_, ErrorCategory::Cancelled); QVERIFY(Elapsed.elapsed() < 1500);
        Worker([&] { Host->Close(); }); QVERIFY(!Host->Snapshot().running_);
    }
    void faults_data()
    {
        QTest::addColumn<QString>("Name");
        for (const auto Name : {"reject", "hang-load", "hang-message", "crash-command", "hang-close", "quit", "hang-load-default", "hang-message-default", "hang-close-default"}) QTest::newRow(Name) << QString(Name);
    }
    void faults()
    {
        QFETCH(QString, Name);
        auto Fault = Name; Fault.remove("-default");
        QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, Approval));
        QWidget Parent; Parent.resize(320, 200); Parent.show(); QVERIFY(QTest::qWaitForWindowExposed(&Parent));
        HostLimits Limits; if (!Name.endsWith("-default")) { Limits.requestMs_ = 500; Limits.heartbeatMs_ = 200; }
        auto Started = Worker([&] { return PluginProcess::Start(host_, Directory.filePath("plugin.json"), Approval, Limits); });
        QVERIFY2(Started.HasValue(), Started.HasValue() ? "" : Started.Failure().code_.c_str());
        auto Host = std::move(Started.Value()); const auto Cleanup = qScopeGuard([&] { Worker([&] { Host.reset(); }); });
        QElapsedTimer Clock; Clock.start(); qint64 LastTick = 0, MaximumGap = 0; unsigned Ticks = 0;
        QTimer Timer; Timer.setInterval(10);
        connect(&Timer, &QTimer::timeout, this, [&] { const auto Now = Clock.elapsed(); MaximumGap = std::max(MaximumGap, Now - LastTick); LastTick = Now; ++Ticks; });
        Timer.start();
        const auto Request = request(Parent, Directory.filePath(Name + ".txt"));
        auto Opened = Worker([&] { return Host->ViewerCall(WireKind::ViewerOpen, Request); });
        if (Fault == "reject")
        {
            QVERIFY(!Opened.HasValue()); QVERIFY(Worker([&] { return Host->Ping(); }).HasValue());
            auto Retry = Request; Retry["file"] = Directory.filePath("ok.txt"); request(Parent, Retry["file"].toString());
            QVERIFY(Worker([&] { return Host->ViewerCall(WireKind::ViewerOpen, Retry); }).HasValue()); return;
        }
        if (Fault == "hang-load") QVERIFY(!Opened.HasValue());
        else
        {
            QVERIFY2(Opened.HasValue(), Opened.HasValue() ? "" : qPrintable(QString::fromStdString(Opened.Failure().code_) + " exit=" + QString::number(Host->Snapshot().exitCode_.value_or(0))));
            if (Fault == "crash-command") QVERIFY(!Worker([&] { return Host->ViewerCall(WireKind::ViewerCommand, {{"command", 3}, {"parameter", 0}}); }).HasValue());
            if (Fault == "hang-close") QVERIFY(!Worker([&] { return Host->ViewerCall(WireKind::ViewerClose, {}); }).HasValue());
            if (Fault == "quit") QVERIFY(PostMessageW(Window(Opened.Value(), "viewer"), WM_APP + 83, 0, 0));
        }
        QTRY_VERIFY_WITH_TIMEOUT(!Worker([&] { return Host->Snapshot(); }).running_, 4000);
        if (Opened.HasValue()) QVERIFY(!IsWindow(Window(Opened.Value(), "viewer")));
        QVERIFY(Host->Snapshot().failure_.has_value());
        if (Fault == "quit") QCOMPARE(Host->Snapshot().exitCode_.value_or(0), 6u);
        qInfo() << "GUI heartbeat during" << Name << "ticks" << Ticks << "maximum gap ms" << MaximumGap;
        qInfo() << "Runtime fault measurements" << Name << Host->Snapshot().runtime_;
        QVERIFY(Ticks > 0); QVERIFY2(MaximumGap < 200, "Plugin failure stalled the caller GUI event loop");
    }
};
int main(int Argc, char** Argv)
{
    TimedApplication App(Argc, Argv);
    if (App.arguments().contains("--probe-dpr")) { std::cout << App.primaryScreen()->devicePixelRatio() << std::endl; return 0; }
    HostedWlxTests Tests; return QTest::qExec(&Tests, Argc, Argv);
}
#include "tstHostedWlx.moc"
