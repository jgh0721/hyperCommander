#include "UniqueLibs/document/documentWorkspaceWidget.hpp"
#include "runtimeVerification.hpp"
#include <fmstyle/ThemeManager.h>
#include <QJsonArray>
#include <QJsonDocument>
#include "commonLibs/document/epub/epubAppearance.hpp"
#include "UniqueLibs/document/hwpStartupTrace.hpp"
#include "hwpProductPolicy.hpp"
#include "UniqueLibs/document/hwpOpenWindow.hpp"
#include <QApplication>
#include <QIcon>
#include <QCommandLineParser>
#include <QTimer>
#include <QCloseEvent>
#include <QStandardPaths>
#include <QFileInfo>
#include <QMessageBox>
#include <memory>
#include <vector>
#include "UniqueLibs/document/documentThemeAdapter.hpp"
#include "UniqueLibs/settings/appSettings.hpp"
#include "HyperCommanderUpdater/updateTransaction.hpp"
#include <Windows.h>

void InitializePortedViewResources();

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
    if (!hc::update::StartupAllowed(hc::update::ExecutableRoot())) {
        MessageBoxW(nullptr, L"업데이트가 진행 중이거나 복구가 필요합니다. HyperCommanderUpdater.exe --recover <설치 폴더>로 복구하십시오.", L"HyperEditor", MB_OK | MB_ICONWARNING); return ERROR_BUSY;
    }
    hc::document::startup::Initialize("editor");
    hc::document::startup::Data().Begin("editor.entryToTitle");
    const auto QtStart = hc::document::startup::Data().Begin("editor.qtApplication");
    QApplication App(Argc, Argv); App.setOrganizationName("HyperCommander"); App.setApplicationName("HyperEditor");
    InitializePortedViewResources(); App.setWindowIcon(QIcon(":/icons/app/hypereditor.svg"));
    hc::document::startup::Data().End(QtStart);
    QCommandLineParser Parser; Parser.addHelpOption();
    Parser.addOption(QCommandLineOption("rhwp-edit", "Edit one HWP/HWPX document"));
    Parser.addOption(QCommandLineOption("rhwp-edit-copy", "Edit a provider copy with Save As only"));
    Parser.addOption(QCommandLineOption("rhwp-view", "Open one HWP/HWPX in the isolated read-only viewer"));
    Parser.addOption(QCommandLineOption("document-appearance", "Initial Watercolor appearance supplied by Commander", "base64"));
    Parser.addOption(QCommandLineOption("verify-epub-runtime", "Verify the installed EPUB reader without changing the book", "file"));
    Parser.addOption(QCommandLineOption("verify-runtime", "Verify installed codecs, fonts and document saves using temporary files"));
    Parser.addOption(QCommandLineOption("config-dir", "Settings directory", "path"));
    Parser.addOption(QCommandLineOption("portable", "Use the executable directory (default)"));
    Parser.addOption(QCommandLineOption("theme", "system/light/dark", "mode"));
    Parser.addPositionalArgument("files", "PDF, text and image files", "[files...]"); Parser.process(App);
    const auto Config = hc::settings::DocumentPaths();
    if (!Config.error_.isEmpty()) { QMessageBox::critical(nullptr, QObject::tr("설정 경로 오류"), Config.error_); return 11; }
    hc::document::DocumentThemeAdapter::Install(App);
    QJsonObject ParentAppearance;
    if(Parser.isSet("document-appearance")) {
        const auto Encoded=Parser.value("document-appearance").toLatin1(); if(Encoded.size()>16384)return 9;
        const auto Decoded=QJsonDocument::fromJson(QByteArray::fromBase64(Encoded,QByteArray::AbortOnBase64DecodingErrors));
        if(!Decoded.isObject() || !hc::document::epubui::ApplyAppearance(Decoded.object()))return 9;
        ParentAppearance=Decoded.object();
    }
    if (Parser.isSet("verify-epub-runtime")) return VerifyEpubRuntime(QFileInfo(Parser.value("verify-epub-runtime")).absoluteFilePath());
    if (Parser.isSet("verify-runtime")) return VerifyDocumentRuntime();
    const bool RhwpEdit = Parser.isSet("rhwp-edit") || Parser.isSet("rhwp-edit-copy");
    if (Parser.isSet("rhwp-view") || RhwpEdit) {
        if ((Parser.isSet("rhwp-view") && RhwpEdit) || (Parser.isSet("rhwp-edit") && Parser.isSet("rhwp-edit-copy"))) return 10;
        if (Parser.positionalArguments().size() != 1) return 10;
        const auto Path = QFileInfo(Parser.positionalArguments().first()).absoluteFilePath();
        const auto Format = QFileInfo(Path).suffix().toLower();
        if (Format != "hwp" && Format != "hwpx") return 10;
        if (Parser.isSet("theme") && !hc::document::DocumentThemeAdapter::SetMode(Parser.value("theme"))) return 9;
        auto Runtime = hc::document::MakeProductHwpRuntime(QCoreApplication::applicationDirPath(),
            QByteArray(HC_RHWP_APPROVED_INVENTORY_SHA256));
        if(!RhwpEdit){
            Workspace Window(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)+"/recovery/viewer",true);
            Window.ConfigureHwp(std::move(Runtime));
            QObject::connect(&Window,&hc::document::DocumentWorkspaceWidget::StateChanged,&Window,[&Window]{
                if(Window.Snapshot()["closeRequested"].toBool())Window.close();
            });
            Window.show();
            Window.Handle({{"action","open"},{"path",Path}});return App.exec();
        }
        hc::document::HwpOpenWindow Window(Path,
            QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/rhwp", std::move(Runtime), {}, RhwpEdit, !Parser.isSet("rhwp-edit-copy"));
        QObject::connect(&Window, &hc::document::HwpOpenWindow::Closed, &App, &QCoreApplication::quit);
        QObject::connect(&App,&QCoreApplication::aboutToQuit,&App,[] {
            // The Ready observation has completed; all owners drain before normal quit.
            const auto Trace = hc::document::startup::Data().Snapshot();
            hc::document::startup::Write(Trace,"editor",QCoreApplication::applicationPid());
        });
        Window.show(); QTimer::singleShot(0, &Window, &hc::document::HwpOpenWindow::Open);
        return App.exec();
    }
    const auto RecoveryRoot = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/recovery/editor";
    hc::settings::JsonStore Settings(hc::settings::UnifiedConfig::FileName(Config.directory_),
        {{"schemaVersion", 2}}, nullptr, hc::settings::JsonStore::Scope::Product);
    const auto Appearance = Settings.Object()["appearance"].toObject();
    const auto General = Settings.Object()["general"].toObject();
    auto& Theme = fm::style::ThemeManager::instance();
    Theme.setColorScheme(fm::style::ColorScheme::fromJson(Appearance["watercolorTheme"].toObject()).value_or(fm::style::ColorScheme{}));
    Theme.setMonoFont(General["monoFontFamily"].toString("Cascadia Mono"), General["monoFontPx"].toInt(12));
    hc::document::DocumentThemeAdapter::SetMode(General["themeMode"].toString("system"));
    if (Parser.isSet("theme") && !hc::document::DocumentThemeAdapter::SetMode(Parser.value("theme"))) return 9;
    std::vector<std::unique_ptr<Workspace>> Windows;
    auto Files = Parser.positionalArguments();
    if (Files.isEmpty()) Files.append(QString{});
    Windows.reserve(Files.size());
    for (const auto& File : Files) {
        auto Window = std::make_unique<Workspace>(RecoveryRoot, false);
        Window->ConfigureHwp(hc::document::MakeProductHwpRuntime(QCoreApplication::applicationDirPath(),QByteArray(HC_RHWP_APPROVED_INVENTORY_SHA256)));
        if (Appearance.contains("documentWorkspace") && !Window->ApplyChromeSettings(Appearance["documentWorkspace"].toObject()))
            Window->ShowShellError(QObject::tr("문서 표시 설정을 적용하지 못하여 기본 표시를 사용합니다."));
        if(!ParentAppearance.isEmpty()) hc::document::epubui::ApplyAppearance(ParentAppearance);
        auto* Current = Window.get();
        QObject::connect(Current, &hc::document::DocumentWorkspaceWidget::ChromeSettingsChanged, &App,
            [Current, &Settings](const QJsonObject& Chrome) {
                auto Next = Settings.Object()["appearance"].toObject(); Next["documentWorkspace"] = Chrome;
                auto Root = Settings.Object(); Root["appearance"] = Next;
                if (!Settings.Replace(Root)) Current->ShowShellError(QObject::tr("표시 설정을 저장하지 못했습니다."));
                else Current->ShowShellError({});
            });
        QObject::connect(Current, &hc::document::DocumentWorkspaceWidget::StateChanged, Current, [Current] {
            const auto Documents = Current->Snapshot()["documents"].toArray();
            for(const auto& Item : Documents) {
                const auto Record = Item.toObject();
                if(Record["view"].toObject()["viewId"] != "epub") continue;
                const auto State = !Record["error"].toString().isEmpty() ? QObject::tr("열기 실패")
                    : Record["view"].toObject()["ready"].toBool() ? QObject::tr("읽기 전용") : QObject::tr("불러오는 중");
                Current->setWindowTitle(Record["displayName"].toString() + " — " + State + " — HyperEditor"); return;
            }
            Current->setWindowTitle("HyperEditor");
        });
        Current->show();
        if (!File.isEmpty() && Current->Handle({{"action", "open"}, {"path", QFileInfo(File).absoluteFilePath()}}).contains("error"))
            Current->ShowShellError(QObject::tr("문서 한도 때문에 파일을 열지 못했습니다: %1").arg(File));
        Windows.push_back(std::move(Window));
    }
    return App.exec();
}
