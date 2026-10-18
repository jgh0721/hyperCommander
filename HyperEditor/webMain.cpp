#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <shellapi.h>
#include "UniqueLibs/document/documentWebPeer.hpp"
#include "UniqueLibs/document/hwpPackageAdmission.hpp"
#include "commonLibs/layout/productLayout.hpp"
#include "hwpRuntimePolicy.hpp"
#include "UniqueLibs/document/hwpHelperDocument.hpp"
#include "UniqueLibs/document/hwpStartupTrace.hpp"
#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QFile>
#include <QCryptographicHash>
#include <QJsonArray>

namespace
{
    QJsonObject verifyEmbeddedBundle(const QByteArray& Pin)
    {
        using namespace hc::document;
        QJsonObject Result{{"schemaVersion", 1}, {"diagnosticOnly", true}, {"bytesVerified", false},
            {"productApproved", false}, {"rendererCreated", false}};
        const auto Fail = [&](const QString& Error) { Result["error"] = Error; return Result; };
        if (Pin.size() != 64 || QByteArray::fromHex(Pin).toHex() != Pin) return Fail("rhwp-inspection-pin");
        const QString Root = QStringLiteral(":/hc/rhwp/bundle");
        QFile Manifest(Root + "/asset-manifest.json");
        if (!Manifest.open(QIODevice::ReadOnly)) return Fail("rhwp-inspection-resource-missing");
        const auto Bytes = Manifest.read(256 * 1024 + 1);
        if (Manifest.error() != QFileDevice::NoError || !Manifest.atEnd() || Bytes.size() > 256 * 1024)
            return Fail("rhwp-inspection-budget");
        const auto Hash = QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256).toHex();
        Result["manifestSha256"] = QString::fromLatin1(Hash);
        if (Hash != Pin) return Fail("rhwp-inspection-manifest-hash");
        QJsonParseError Parse; const auto Json = QJsonDocument::fromJson(Bytes, &Parse);
        if (Parse.error != QJsonParseError::NoError || !Json.isObject()) return Fail("rhwp-inspection-json");
        const auto Object = Json.object();
        if (Object["schemaVersion"] != QJsonValue(1) || !Object["files"].isArray()
            || Object["files"].toArray().isEmpty() || Object["files"].toArray().size() > 512)
            return Fail("rhwp-inspection-schema");
        QMap<QString, QByteArray> Pins;
        for (const auto& Value : Object["files"].toArray()) {
            if (!Value.isObject()) return Fail("rhwp-inspection-entry");
            const auto Entry = Value.toObject();
            if (!Entry["path"].isString() || !Entry["sha256"].isString() || Pins.contains(Entry["path"].toString()))
                return Fail("rhwp-inspection-entry");
            Pins.insert(Entry["path"].toString(), Entry["sha256"].toString().toLatin1());
        }
        QString Error;
        if (!HwpBundle::LoadPinned(Root, Pins, Error)) return Fail(Error);
        Result["bytesVerified"] = true; Result["fileCount"] = Pins.size();
        Result["releaseEligible"] = Object["releaseEligible"];
        // Caller-selected inspection pins never become native release policy.
        Result["productApproved"] = bool(LoadApprovedHwpPackage(Root, QByteArray(HC_RHWP_APPROVED_MANIFEST_SHA256), Error));
        Result["productPolicyError"] = Error;
        return Result;
    }
    // A hidden attachment surface only: no page, renderer, input or document UI.
    // The native caller must keep the parent binding alive through startup and
    // retirement. Rebinding requires a future generation-aware typed contract.
    class ColdSurface final
    {
    public:
        ~ColdSurface() { if(window_) DestroyWindow(window_); }
        bool Create(HWND Parent, DWORD OwnerPid)
        {
            parent_ = Parent; ownerPid_ = OwnerPid;
            DWORD Pid = 0; GetWindowThreadProcessId(parent_, &Pid);
            if(Pid != ownerPid_) return false;
            WNDCLASSW Class{}; Class.lpfnWndProc = windowProc;
            Class.hInstance = GetModuleHandleW(nullptr); Class.lpszClassName = L"HyperCommander.RhwpColdSurface";
            if(!RegisterClassW(&Class)) return false;
            if(!CreateWindowExW(WS_EX_NOPARENTNOTIFY, Class.lpszClassName, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                0, 0, 0, 0, parent_, nullptr, Class.hInstance, this)) return false;
            return Refresh();
        }
        HWND Window() const { return window_; }
        bool Refresh()
        {
            DWORD Pid = 0; GetWindowThreadProcessId(parent_, &Pid);
            if(!window_ || Pid != ownerPid_ || GetParent(window_) != parent_) return false;
            RECT ParentRect{}, ChildRect{};
            if(!GetClientRect(parent_, &ParentRect) || !GetClientRect(window_, &ChildRect)) return false;
            if(ParentRect.right != ChildRect.right || ParentRect.bottom != ChildRect.bottom)
                return SetWindowPos(window_, nullptr, 0, 0, ParentRect.right, ParentRect.bottom,
                    SWP_NOACTIVATE | SWP_NOZORDER) != FALSE;
            return true;
        }
    private:
        static LRESULT CALLBACK windowProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
        {
            auto* Surface = reinterpret_cast<ColdSurface*>(GetWindowLongPtrW(Window, GWLP_USERDATA));
            if(Message == WM_NCCREATE) {
                Surface = static_cast<ColdSurface*>(reinterpret_cast<CREATESTRUCTW*>(LParam)->lpCreateParams);
                Surface->window_ = Window;
                SetWindowLongPtrW(Window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(Surface));
            }
            if(Message == WM_NCDESTROY && Surface) {
                // Never accept a later recycled HWND as the original surface.
                Surface->window_ = nullptr; SetWindowLongPtrW(Window, GWLP_USERDATA, 0);
            }
            return DefWindowProcW(Window, Message, WParam, LParam);
        }
        HWND window_ = nullptr, parent_ = nullptr;
        DWORD ownerPid_ = 0;
    };
    bool ExpectedJobPolicy()
    {
        BOOL InJob = FALSE;
        JOBOBJECT_BASIC_UI_RESTRICTIONS Ui{};
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION Job{};
        return IsProcessInJob(GetCurrentProcess(), nullptr, &InJob) && InJob
            && QueryInformationJobObject(nullptr, JobObjectBasicUIRestrictions, &Ui, sizeof(Ui), nullptr)
            && !Ui.UIRestrictionsClass
            && QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &Job, sizeof(Job), nullptr)
            && (Job.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE)
            && !(Job.BasicLimitInformation.LimitFlags & (JOB_OBJECT_LIMIT_BREAKAWAY_OK | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK))
            && (Job.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_ACTIVE_PROCESS)
            && (Job.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_PROCESS_MEMORY)
            && (Job.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_JOB_MEMORY)
            && Job.ProcessMemoryLimit >= 256ULL * 1024 * 1024 && Job.ProcessMemoryLimit <= 8ULL * 1024 * 1024 * 1024
            && Job.JobMemoryLimit >= 256ULL * 1024 * 1024 && Job.JobMemoryLimit <= 8ULL * 1024 * 1024 * 1024
            && Job.BasicLimitInformation.ActiveProcessLimit >= 2 && Job.BasicLimitInformation.ActiveProcessLimit <= 64;
    }
}
int main(int Argc, char** Argv)
{
    using namespace hc::document;
    startup::Initialize("helper");startup::Data().Begin("helper.entryToTransportReady");
    // Inspect the original wide argv before Qt can consume WebEngine switches.
    int Count = 0;
    auto* Raw = CommandLineToArgvW(GetCommandLineW(), &Count);
    if (!Raw) return 2;
    QStringList Args;
    for (int Index = 0; Index < Count; ++Index) Args.append(QString::fromWCharArray(Raw[Index]));
    LocalFree(Raw);
    if (Args.size() >= 2 && Args[1] == "--verify-bundle-resources") {
        if (Args.size() != 3) return 2;
        // Offline byte inspection only: no QApplication, scheme, page or IPC.
        QCoreApplication App(Argc, Argv);
        const auto Report = verifyEmbeddedBundle(Args[2].toLatin1());
        const auto Bytes = QJsonDocument(Report).toJson(QJsonDocument::Compact) + '\n';
        DWORD Written = 0;
        if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), Bytes.constData(), DWORD(Bytes.size()), &Written, nullptr)
            || Written != DWORD(Bytes.size())) return 3;
        return Report["bytesVerified"].toBool() ? 0 : 1;
    }
    if (!HwpWebArgumentsAllowed(Args) || !HwpWebEnvironmentAllowed() || Args.size() != 10
        || Args[1] != "--web-owner" || Args[2] != "--pipe" || Args[4] != "--connection"
        || Args[6] != "--parent-process" || Args[8] != "--parent"
        || !Args[3].startsWith(QStringLiteral(R"(\\.\pipe\hc-web-owner-)"))
        || !ExpectedJobPolicy()) return 2;
    bool ValidParent = false;
    const auto ParentValue = Args[9].toULongLong(&ValidParent);
    if(!ValidParent || QString::number(ParentValue) != Args[9]
        || ParentValue > UINTPTR_MAX) return 2;
    const auto Parent = static_cast<std::uintptr_t>(ParentValue);
    bool ValidPid = false;
    const auto OwnerPid = Args[7].toUInt(&ValidPid);
    if (!ValidPid || !OwnerPid || !RegisterHwpScheme()) return 2;
    startup::Scope QtStartup("helper.qtApplication");
    QApplication App(Argc, Argv);
    QtStartup.End();
    App.setOrganizationName("HyperCommander"); App.setApplicationName("HyperEditorWeb");
    App.setQuitOnLastWindowClosed(false);
    DocumentWebPeer Peer;
    { startup::Scope Connect("helper.pipeConnect");
      if (!Peer.Connect(Args[3], Args[5].toLatin1(), OwnerPid, Parent)) return 3; }
    ColdSurface Surface;
    if(Parent && !Surface.Create(reinterpret_cast<HWND>(Parent), OwnerPid)) return 4;
    QTimer SurfaceTimer;
    if(Parent) {
        SurfaceTimer.setInterval(50);
        QObject::connect(&SurfaceTimer, &QTimer::timeout, &App, [&] { if(!Surface.Refresh()) App.quit(); });
        SurfaceTimer.start();
    }
    QString PackageError;
    // Both product entry points use this helper and the same immutable resources.
#ifdef HC_RHWP_EMBEDDED_BUNDLE
    const auto BundleRoot = QStringLiteral(":/hc/rhwp/bundle");
#else
    const auto BundleRoot = QCoreApplication::applicationDirPath() + '/' + hc::layout::RhwpBundle;
#endif
    auto Bundle = LoadApprovedHwpPackage(BundleRoot,
        QByteArray(HC_RHWP_APPROVED_MANIFEST_SHA256), PackageError);
    const auto EditPolicy = Bundle ? QJsonDocument::fromJson(Bundle->Bytes(QUrl("rhwp://bundle/editor/policy.json"))).object() : QJsonObject{};
    const bool AllowEdit = EditPolicy.size() == 2 && EditPolicy["schemaVersion"] == QJsonValue(1) && EditPolicy["edit"] == QJsonValue(true);
    HwpHelperDocument Document(Bundle, reinterpret_cast<std::uintptr_t>(Surface.Window()), 30000, AllowEdit);
    QTimer InputTimer; InputTimer.setInterval(100);
    QObject::connect(&InputTimer, &QTimer::timeout, &App, [&] { Document.Refresh(); });
    InputTimer.start();
    // InputVerified acknowledges only a bounded byte snapshot, never document Ready.
    // Runtime Ready authenticates transport only; DocumentStart creates the page.
    // InputVerified never substitutes for a validated first-frame status.
    int TraceExports=0;
    if (!Peer.Ready(reinterpret_cast<std::uintptr_t>(Surface.Window()), [&](const QByteArray& Request) {
        auto Reply = [](const QJsonObject& Object) { return QJsonDocument(Object).toJson(QJsonDocument::Compact); };
        if (Request.size() > 300 * 1024) return Reply({{"error", "rhwp-control-budget"}});
        QJsonParseError Parse;
        const auto Json = QJsonDocument::fromJson(Request, &Parse);
        if (Parse.error != QJsonParseError::NoError || !Json.isObject()) return Reply({{"error", "rhwp-control-json"}});
        const auto Object = Json.object();
        if (Object.value("type").toString().startsWith("Input")) return Reply(Document.Handle(Object));
        if (Object.value("type").toString().startsWith("Document") && Request.size() <= 4096)
            return Reply(Document.Handle(Object));
        if (Request.size() > 4096) return Reply({{"error", "rhwp-control-budget"}});
        if (Object.size() != 2 || Object.value("version") != QJsonValue(1) || !Object.value("type").isString())
            return Reply({{"error", "rhwp-control-schema"}});
        if(Object.value("type") == QJsonValue("RuntimeStartupTrace") && startup::Enabled())
            return ++TraceExports<=5?Reply(startup::Data().Snapshot()):Reply({{"error","trace-export-budget"}});
        if (Object.value("type") == QJsonValue("RuntimeStatus"))
            return Reply({{"type", "RuntimeStatus"}, {"version", 1}, {"transportReady", true},
                {"packageApproved", bool(Bundle)}, {"packageError", PackageError},
                {"rendererCreated", Document.RendererCreated()}, {"documentCommands", bool(Bundle)}, {"inputTransfer", bool(Bundle)}, {"surfaceAttached", Surface.Window() != nullptr}});
        if (Object.value("type") == QJsonValue("Shutdown")) {
            QTimer::singleShot(0, &App, &QCoreApplication::quit);
            return Reply({{"type", "ShutdownAck"}, {"version", 1}});
        }
        return Reply({{"error", "rhwp-command-unavailable"}});
    }, [&] { App.quit(); })) return 4;
    startup::Data().EndNamed("helper.entryToTransportReady");
    return App.exec();
}
