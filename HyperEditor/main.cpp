#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "UniqueLibs/document/documentWorkspaceWidget.hpp"
#include "runtimeVerification.hpp"
#include "UniqueLibs/document/hwpStartupTrace.hpp"
#include "hwpProductPolicy.hpp"
#include "UniqueLibs/document/hwpOpenWindow.hpp"
#include "UniqueLibs/document/hwpHostSession.hpp"
#include "commonLibs/document/documentHostProtocol.hpp"
#include <QApplication>
#include <QWindow>
#include <QCommandLineParser>
#include <QLocalSocket>
#include <QJsonDocument>
#include <QTimer>
#include <QCloseEvent>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include "UniqueLibs/document/documentThemeAdapter.hpp"
#include "UniqueLibs/settings/appSettings.hpp"
#include <charconv>

namespace
{
    class Workspace final : public hc::document::DocumentWorkspaceWidget
    {
    public:
        using hc::document::DocumentWorkspaceWidget::DocumentWorkspaceWidget;
    protected:
        void closeEvent(QCloseEvent* Event) override { if (RequestClose()) Event->accept(); else Event->ignore(); }
    };
}
int main(int Argc, char** Argv)
{
    hc::document::startup::Initialize("editor");
    hc::document::startup::Data().Begin("editor.entryToTitle");
    const auto QtStart = hc::document::startup::Data().Begin("editor.qtApplication");
    // Align DPI awareness before Qt creates any native GUI object. Cross-process
    // SetParent cannot silently reset the helper's awareness to another mode.
    std::uintptr_t ParentValue = 0;
    for (int Index = 1; Index + 1 < Argc; ++Index) if (std::string_view(Argv[Index]) == "--parent")
        std::from_chars(Argv[Index + 1], Argv[Index + 1] + std::strlen(Argv[Index + 1]), ParentValue);
    if (ParentValue) SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(reinterpret_cast<HWND>(ParentValue)));
    QApplication App(Argc, Argv); App.setOrganizationName("HyperCommander"); App.setApplicationName("HyperEditor");
    hc::document::startup::Data().End(QtStart);
    QCommandLineParser Parser; Parser.addHelpOption();
    for (const auto& Name : {"pipe", "connection", "parent", "parent-process"}) Parser.addOption(QCommandLineOption(Name, Name, "value"));
    Parser.addOption(QCommandLineOption("viewer-host", "Internal viewer workspace"));
    Parser.addOption(QCommandLineOption("rhwp-edit", "Edit one HWP/HWPX document"));
    Parser.addOption(QCommandLineOption("rhwp-edit-copy", "Edit a provider copy with Save As only"));
    Parser.addOption(QCommandLineOption("rhwp-view", "Open one HWP/HWPX in the isolated read-only viewer"));
    Parser.addOption(QCommandLineOption("verify-runtime", "Verify installed codecs, fonts and document saves using temporary files"));
    Parser.addOption(QCommandLineOption("config-dir", "Settings directory", "path"));
    Parser.addOption(QCommandLineOption("portable", "Use the executable directory (default)"));
    Parser.addOption(QCommandLineOption("theme", "system/light/dark", "mode"));
    Parser.addPositionalArgument("files", "PDF, text and image files", "[files...]"); Parser.process(App);
    const bool Hosted = Parser.isSet("viewer-host");
    const auto Config = hc::settings::DocumentPaths();
    if (!Config.error_.isEmpty()) { QMessageBox::critical(nullptr, QObject::tr("설정 경로 오류"), Config.error_); return 11; }
    hc::document::DocumentThemeAdapter::Install(App);
    if (Parser.isSet("verify-runtime")) return VerifyDocumentRuntime();
    const bool RhwpEdit = Parser.isSet("rhwp-edit") || Parser.isSet("rhwp-edit-copy");
    if (Parser.isSet("rhwp-view") || RhwpEdit) {
        if ((Parser.isSet("rhwp-view") && RhwpEdit) || (Parser.isSet("rhwp-edit") && Parser.isSet("rhwp-edit-copy"))) return 10;
        if (Hosted || Parser.isSet("pipe") || Parser.isSet("parent") || Parser.isSet("parent-process")
            || Parser.isSet("connection") || Parser.positionalArguments().size() != 1) return 10;
        const auto Path = QFileInfo(Parser.positionalArguments().first()).absoluteFilePath();
        const auto Format = QFileInfo(Path).suffix().toLower();
        if (Format != "hwp" && Format != "hwpx") return 10;
        if (Parser.isSet("theme") && !hc::document::DocumentThemeAdapter::SetMode(Parser.value("theme"))) return 9;
        auto Runtime = hc::document::MakeProductHwpRuntime(QCoreApplication::applicationDirPath(),
            QByteArray(HC_RHWP_APPROVED_INVENTORY_SHA256));
        hc::document::HwpOpenWindow Window(Path,
            QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/rhwp", std::move(Runtime), {}, RhwpEdit, !Parser.isSet("rhwp-edit-copy"));
        QObject::connect(&App,&QCoreApplication::aboutToQuit,&App,[] {
            // The Ready observation has completed; all owners drain before normal quit.
            const auto Trace = hc::document::startup::Data().Snapshot();
            hc::document::startup::Write(Trace,"editor",QCoreApplication::applicationPid());
        });
        Window.show(); QTimer::singleShot(0, &Window, &hc::document::HwpOpenWindow::Open);
        return App.exec();
    }
    const auto RecoveryRoot = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + (Hosted ? "/recovery/viewer" : "/recovery/editor");
    Workspace Window(RecoveryRoot, Hosted);
    hc::settings::JsonStore Settings(hc::settings::UnifiedConfig::FileName(Config.directory_),
        {{"schemaVersion", 2}}, nullptr, hc::settings::JsonStore::Scope::Product);
    const auto Appearance = Settings.Object()["appearance"].toObject();
    if (Appearance.contains("documentWorkspace") && !Window.ApplyChromeSettings(Appearance["documentWorkspace"].toObject()))
        Window.ShowShellError(QObject::tr("문서 표시 설정을 적용하지 못하여 기본 표시를 사용합니다."));
    if (Parser.isSet("theme") && !hc::document::DocumentThemeAdapter::SetMode(Parser.value("theme"))) return 9;
    QObject::connect(&Window, &hc::document::DocumentWorkspaceWidget::ChromeSettingsChanged, &App, [&](const QJsonObject& Chrome) {
        auto Next = Settings.Object()["appearance"].toObject(); Next["documentWorkspace"] = Chrome;
        auto Root = Settings.Object(); Root["appearance"] = Next;
        if (!Settings.Replace(Root)) Window.ShowShellError(QObject::tr("표시 설정을 저장하지 못했습니다."));
        else Window.ShowShellError({});
    });
    JOBOBJECT_BASIC_UI_RESTRICTIONS Restrictions{};
    if (Hosted && QueryInformationJobObject(nullptr, JobObjectBasicUIRestrictions, &Restrictions, sizeof(Restrictions), nullptr))
        Window.SetClipboardBroker((Restrictions.UIRestrictionsClass & (JOB_OBJECT_UILIMIT_READCLIPBOARD | JOB_OBJECT_UILIMIT_WRITECLIPBOARD)) != 0);
    hc::document::HwpHostSession Rhwp(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/rhwp",
        hc::document::MakeProductHwpRuntime(QCoreApplication::applicationDirPath(), QByteArray(HC_RHWP_APPROVED_INVENTORY_SHA256)));
    QLocalSocket Socket; QByteArray Buffered; std::array<unsigned char, 16> Connection{}; std::uint64_t LastRequest = 0;
    HANDLE ParentProcess = nullptr;
    DWORD InputThread = 0;
    const auto QuitAfterCheckpoint = [&] {
        Rhwp.Shutdown(); Window.Checkpoint();
        auto* Drain = new QTimer(&App); Drain->setInterval(20);
        QObject::connect(Drain, &QTimer::timeout, &App, [&] { if (Window.IsIdle() && Rhwp.IsIdle()) App.quit(); }); Drain->start();
        QTimer::singleShot(1500, &App, &QCoreApplication::quit);
    };
    if (Hosted)
    {
        bool ValidParent = false; const auto ParentId = Parser.value("parent-process").toUInt(&ValidParent);
        const auto Parent = reinterpret_cast<HWND>(ParentValue); DWORD Owner = 0;
        const auto Hex = Parser.value("connection").toLatin1(); const auto Raw = QByteArray::fromHex(Hex);
        if (!ValidParent || !ParentId || !Parent || !GetWindowThreadProcessId(Parent, &Owner) || Owner != ParentId
            || Hex.size() != 32 || Raw.size() != 16 || Raw.toHex() != Hex
            || !Parser.value("pipe").startsWith("\\\\.\\pipe\\hc-hyperviewer-")) return 2;
        std::memcpy(Connection.data(), Raw.constData(), 16);
        ParentProcess = OpenProcess(SYNCHRONIZE, FALSE, ParentId); if (!ParentProcess) return 3;
        SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(Parent));
        // The hosted workspace is a child of HC, never a foreground shell.
        // Native input creation must not activate the original Qt root while
        // its owning HC dialog is still hidden for preparation.
        Window.setWindowFlags(Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
        Window.setAttribute(Qt::WA_ShowWithoutActivating);
        const auto Child = reinterpret_cast<HWND>(Window.winId());
        // Let Qt track the foreign native parent. Raw SetParent alone can be
        // undone by Qt's top-level window state while processing WM messages.
        auto* Foreign = QWindow::fromWinId(ParentValue);
        if (!Foreign) { CloseHandle(ParentProcess); return 4; }
        Foreign->QObject::setParent(&App);
        Window.windowHandle()->setParent(Foreign);
        if (GetParent(Child) != Parent) { CloseHandle(ParentProcess); return 4; }
        // The helper stays hidden during background preparation. Connect its
        // input queue only when the HC surface is actually presented.
        // QWidget began as a top-level shell. Reset its native child position
        // after showing so a cached desktop position cannot offset the editor.
        Window.windowHandle()->setPosition(QPoint(0,0));
        Socket.setReadBufferSize(hc::document::HostPayloadLimit + sizeof(hc::document::HostFrame));
        QObject::connect(&Socket, &QLocalSocket::connected, &App, [&, ParentId, Child] {
            ULONG Server = 0;
            if (!GetNamedPipeServerProcessId(reinterpret_cast<HANDLE>(Socket.socketDescriptor()), &Server) || Server != ParentId) { App.exit(5); return; }
            hc::document::HostFrame Ready; Ready.type_ = 0; Ready.connection_ = Connection; Ready.window_ = reinterpret_cast<std::uintptr_t>(Child);
            Socket.write(reinterpret_cast<const char*>(&Ready), sizeof(Ready));
        });
        QObject::connect(&Socket, &QLocalSocket::readyRead, &App, [&] {
            Buffered.append(Socket.readAll());
            if (Buffered.size() > hc::document::HostPayloadLimit + sizeof(hc::document::HostFrame)) { App.exit(6); return; }
            while (Buffered.size() >= sizeof(hc::document::HostFrame))
            {
                hc::document::HostFrame Header; std::memcpy(&Header, Buffered.constData(), sizeof(Header));
                if (!hc::document::ValidFrame(Header, Connection) || Header.type_ != 1 || Header.requestId_ != LastRequest + 1) { App.exit(7); return; }
                if (Buffered.size() < sizeof(Header) + Header.length_) return;
                const auto Bytes = Buffered.mid(sizeof(Header), Header.length_); Buffered.remove(0, sizeof(Header) + Header.length_);
                QJsonParseError Error; const auto Request = QJsonDocument::fromJson(Bytes, &Error);
                QJsonObject Reply;
                if (Error.error != QJsonParseError::NoError || !Request.isObject()) Reply = {{"error", "document-invalid-json"}};
                else if (Request.object().value("action") == QJsonValue("hostRuntime") && Request.object().size() == 1) {
                    BOOL InJob = FALSE; JOBOBJECT_EXTENDED_LIMIT_INFORMATION Limits{};
                    JOBOBJECT_BASIC_UI_RESTRICTIONS Ui{};
                    const bool Valid = IsProcessInJob(GetCurrentProcess(), nullptr, &InJob) && InJob
                        && QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &Limits, sizeof(Limits), nullptr)
                        && QueryInformationJobObject(nullptr, JobObjectBasicUIRestrictions, &Ui, sizeof(Ui), nullptr);
                    const auto Runtime = hc::document::MakeProductHwpRuntime(QCoreApplication::applicationDirPath(),
                        QByteArray(HC_RHWP_APPROVED_INVENTORY_SHA256));
                    Reply = {{"version", 1}, {"jobPolicyAvailable", Valid},
                        {"processLimit", Valid ? int(Limits.BasicLimitInformation.ActiveProcessLimit) : 0},
                        {"processMemoryBytes", Valid ? QString::number(Limits.ProcessMemoryLimit) : QString()},
                        {"jobMemoryBytes", Valid ? QString::number(Limits.JobMemoryLimit) : QString()},
                        {"killOnClose", Valid && bool(Limits.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE)},
                        {"breakaway", Valid && bool(Limits.BasicLimitInformation.LimitFlags & (JOB_OBJECT_LIMIT_BREAKAWAY_OK | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK))},
                        {"uiRestricted", Valid && bool(Ui.UIRestrictionsClass)},
                        {"rhwpRuntimeRoot", Runtime.root_},
                        {"rhwpPolicyAvailable", !Runtime.approvedInventorySha256_->isEmpty()},
                        {"rhwpHostCommands", true}, {"rhwpHostEdit", false}};
                } else Reply = Rhwp.Dispatch(Request.object(), [&](const QJsonObject& Command) {
                    return Command.contains("messageType") ? Window.HandleMessage(Command) : Window.Handle(Command);
                });
                auto Payload = QJsonDocument(Reply).toJson(QJsonDocument::Compact);
                // A large valid state must not terminate an editable workspace.
                // Large save content uses immutable, bounded SaveRead chunks.
                if (Payload.size() > hc::document::HostPayloadLimit)
                    Payload = QJsonDocument(QJsonObject{{"error", "document-reply-budget"}}).toJson(QJsonDocument::Compact);
                Header.length_ = Payload.size(); Header.window_ = 0; ++LastRequest;
                Socket.write(reinterpret_cast<const char*>(&Header), sizeof(Header)); Socket.write(Payload);
            }
        });
        QObject::connect(&Socket, &QLocalSocket::disconnected, &App, QuitAfterCheckpoint);
        QObject::connect(&Socket, &QLocalSocket::errorOccurred, &App, [&](auto) { QuitAfterCheckpoint(); });
        Socket.connectToServer(Parser.value("pipe"));
        auto* Monitor = new QTimer(&App); Monitor->setInterval(100);
        QObject::connect(Monitor, &QTimer::timeout, &App, [&, Parent, Monitor] {
            if (WaitForSingleObject(ParentProcess, 0) != WAIT_TIMEOUT || !IsWindow(Parent)) { Monitor->stop(); QuitAfterCheckpoint(); return; }
            const bool Visible = IsWindowVisible(Parent);
            if (Visible && !Window.isVisible()) {
                if (!InputThread) {
                    const auto Thread = GetWindowThreadProcessId(Parent, nullptr);
                    if (Thread != GetCurrentThreadId()) {
                        if (!AttachThreadInput(GetCurrentThreadId(), Thread, TRUE)) { App.exit(4); return; }
                        InputThread = Thread;
                    }
                }
                Window.show(); Window.windowHandle()->setPosition(QPoint(0,0));
                // Showing a QWidget with a foreign QWindow parent can activate
                // its original shell. Keep native activation on the HC root.
                Window.Handle({{"action", "focus"}});
            } else if (!Visible && Window.isVisible()) {
                // Commander may keep this hidden workspace warm. Detach first so
                // hiding cannot move focus in the shared input queue.
                if (InputThread) AttachThreadInput(GetCurrentThreadId(), std::exchange(InputThread, 0), FALSE);
                Window.hide();
            }
        }); Monitor->start();
    }
    else
    {
        Window.show();
        for (const auto& File : Parser.positionalArguments())
            if (Window.Handle({{"action", "open"}, {"path", QFileInfo(File).absoluteFilePath()}}).contains("error"))
                Window.ShowShellError(QObject::tr("문서 한도 때문에 파일을 열지 못했습니다: %1").arg(File));
    }
    const auto Result = App.exec(); if (InputThread) AttachThreadInput(GetCurrentThreadId(), InputThread, FALSE); if (ParentProcess) CloseHandle(ParentProcess); return Result;
}
