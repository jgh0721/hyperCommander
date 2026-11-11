#include "stdafx.h"
#include "UniqueLibs/document/documentWorkspaceWidget.hpp"
#include "UniqueLibs/fonts/startupFontWarmup.hpp"
#include "UniqueLibs/ProductDefaults_Edit.hpp"
#include "UniqueLibs/qtMessageDialog.hpp"
#include "runtimeVerification.hpp"
#include "UniqueLibs/bundledFonts.hpp"
#include "editorInstance.hpp"
#include <fmstyle/ThemeManager.h>
#include "UniqueLibs/productTheme.hpp"
#include "UniqueLibs/qtTranslation.hpp"
#include <QJsonArray>
#include <QCryptographicHash>
#include <QDir>
#include <QSet>
#include <QJsonDocument>
#include "commonLibs/document/epub/epubAppearance.hpp"
#include "UniqueLibs/document/hwpStartupTrace.hpp"
#include "UniqueLibs/document/documentStallTrace.hpp"
#include "hwpProductPolicy.hpp"
#include "UniqueLibs/document/hwpOpenWindow.hpp"
#include <QApplication>
#include <QIcon>
#include <QCommandLineParser>
#include <QTimer>
#include <QCloseEvent>
#include <QStandardPaths>
#include <QFileInfo>
#include <QFile>
#include <QTemporaryDir>
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
        std::function<void()> closed_;
        bool startupPainted_ = false;
    protected:
        void paintEvent(QPaintEvent* Event) override {
            DocumentWorkspaceWidget::paintEvent(Event);
            if (!startupPainted_) { startupPainted_ = true; hc::document::startup::Data().Aggregate("editor.firstPaint", hc::document::startup::Data().Now()); }
        }
        void closeEvent(QCloseEvent* Event) override { if (RequestClose()) {Event->accept();if(closed_)closed_();} else Event->ignore(); }
    };
}
int main(int Argc, char** Argv)
{
    hc::fonts::StartupFontWarmup FontWarmup(Argc, Argv);
    if (!hc::update::StartupAllowed(hc::update::ExecutableRoot())) {
        MessageBoxW(nullptr, L"업데이트가 진행 중이거나 복구가 필요합니다. HyperCommanderUpdater.exe --recover <설치 폴더>로 복구하십시오.", L"HyperEditor", MB_OK | MB_ICONWARNING); return ERROR_BUSY;
    }
    hc::document::startup::Initialize("editor");
    hc::document::startup::Data().Begin("editor.entryToTitle");
    const auto QtStart = hc::document::startup::Data().Begin("editor.qtApplication");
    QApplication App(Argc, Argv); App.setOrganizationName("HyperCommander"); App.setApplicationName("HyperEditor"); hc::ui::InstallQtTranslation(App);
    FontWarmup.WatchFirstWindow();
    hc::document::startup::Scope ResourcesTrace("editor.resources"); InitializePortedViewResources(); ResourcesTrace.End(); App.setWindowIcon(QIcon(":/icons/app/hypereditor.svg"));
    hc::document::startup::Data().End(QtStart);
    // HC_UI_STALL_TRACE=<file>: GUI thread stalls and delayed key input, for diagnosis only.
    hc::document::DocumentStallTrace::Install();
    QCommandLineParser Parser; Parser.addHelpOption();
    Parser.addOption(QCommandLineOption("new-hwpx", "Create a new HWPX document in edit mode"));
    Parser.addOption(QCommandLineOption("rhwp-edit", "Edit one HWP/HWPX document"));
    Parser.addOption(QCommandLineOption("rhwp-edit-copy", "Edit a provider copy with Save As only"));
    Parser.addOption(QCommandLineOption("rhwp-view", "Open one HWP/HWPX in the isolated read-only viewer"));
    Parser.addOption(QCommandLineOption("epub-edit", "Open one DRM-free EPUB in edit mode"));
    Parser.addOption(QCommandLineOption("new-epub", "Create a new EPUB (dialog) and open it in edit mode"));
    Parser.addOption(QCommandLineOption("find-in-files", "Restore a find/replace session without applying changes", "base64"));
    Parser.addOption(QCommandLineOption("document-copy", "Open a provider copy with Save As only"));
    Parser.addOption(QCommandLineOption("epub-locator", "Initial EPUB reading position", "cfi"));
    Parser.addOption(QCommandLineOption("epub-chapter", "Initial EPUB spine index", "index"));
    Parser.addOption(QCommandLineOption("document-appearance", "Initial Watercolor appearance supplied by Commander", "base64"));
    Parser.addOption(QCommandLineOption("verify-epub-runtime", "Verify the installed EPUB reader without changing the book", "file"));
    Parser.addOption(QCommandLineOption("verify-runtime", "Verify installed codecs, fonts and document saves using temporary files"));
    Parser.addOption(QCommandLineOption("config-dir", "Settings directory", "path"));
    Parser.addOption(QCommandLineOption("portable", "Use the executable directory (default)"));
    Parser.addOption(QCommandLineOption("theme", "system/light/dark", "mode"));
    Parser.addPositionalArgument("files", "PDF, text and image files", "[files...]"); Parser.process(App);
    const auto Config = hc::settings::DocumentPaths();
    if (!Config.error_.isEmpty()) { hc::ui::MessageDialog::critical(nullptr, QObject::tr("설정 경로 오류"), Config.error_); return 11; }
    hc::document::startup::Scope ThemeTrace("editor.theme");
    hc::document::DocumentThemeAdapter::Install(App);
    // Product tokens, accent and fonts first, so every entry point (HWP, EPUB, editor) opens in the product look.
    hc::document::DocumentThemeAdapter::LoadProductTheme(Config.directory_);
    ThemeTrace.End();
    QJsonObject ParentAppearance;
    if(Parser.isSet("document-appearance")) {
        const auto Encoded=Parser.value("document-appearance").toLatin1(); if(Encoded.size()>16384)return 9;
        const auto Decoded=QJsonDocument::fromJson(QByteArray::fromBase64(Encoded,QByteArray::AbortOnBase64DecodingErrors));
        if(!Decoded.isObject() || !hc::document::epubui::ApplyAppearance(Decoded.object()))return 9;
        ParentAppearance=Decoded.object();
    }
    if (Parser.isSet("verify-epub-runtime")) return VerifyEpubRuntime(QFileInfo(Parser.value("verify-epub-runtime")).absoluteFilePath());
    if (Parser.isSet("verify-runtime")) { hc::fonts::RegisterBundledFonts(); return VerifyDocumentRuntime(); }
    // Keep the template input alive until the dedicated editor has drained and closed.
    QTemporaryDir NewHwpxDirectory;
    const bool NewHwpx = Parser.isSet("new-hwpx");
    if (NewHwpx && (Parser.isSet("rhwp-edit") || Parser.isSet("rhwp-edit-copy") || Parser.isSet("rhwp-view")
        || Parser.isSet("epub-edit") || Parser.isSet("new-epub") || Parser.isSet("find-in-files")
        || Parser.isSet("document-copy") || Parser.isSet("epub-locator") || Parser.isSet("epub-chapter")
        || !Parser.positionalArguments().isEmpty())) return 10;
    const bool RhwpEdit = NewHwpx || Parser.isSet("rhwp-edit") || Parser.isSet("rhwp-edit-copy");
    const bool EpubEdit=Parser.isSet("epub-edit");
    // HyperViewer's Ctrl+N › EPUB: HyperEditor shows the dialog and creates the file; no path is passed in.
    const bool NewEpub=Parser.isSet("new-epub");
    if(NewEpub && (EpubEdit || RhwpEdit || Parser.isSet("rhwp-view") || Parser.isSet("find-in-files") || Parser.isSet("document-copy")
        || Parser.isSet("epub-locator") || Parser.isSet("epub-chapter") || !Parser.positionalArguments().isEmpty()))return 10;
    QJsonObject SearchContext;
    if(Parser.isSet("find-in-files")) {
        const auto Encoded=Parser.value("find-in-files").toLatin1();
        if(Encoded.size()>16384 || Parser.positionalArguments().size()!=1)return 9;
        const auto Json=QJsonDocument::fromJson(QByteArray::fromBase64(Encoded,QByteArray::AbortOnBase64DecodingErrors));
        if(!Json.isObject())return 9; SearchContext=Json.object();
    }
    const auto EpubLocator=Parser.value("epub-locator");bool ChapterValid=true;
    const int EpubChapter=Parser.isSet("epub-chapter")?Parser.value("epub-chapter").toInt(&ChapterValid):-1;
    if((!EpubEdit && (Parser.isSet("epub-locator") || Parser.isSet("epub-chapter"))) || (EpubEdit &&
        (RhwpEdit || Parser.isSet("rhwp-view") || Parser.positionalArguments().size()!=1
        || QFileInfo(Parser.positionalArguments().value(0)).suffix().compare("epub",Qt::CaseInsensitive)!=0
        || !ChapterValid || EpubChapter < -1 || EpubChapter>=4096 || EpubLocator.size()>4096
        || (!EpubLocator.isEmpty() && (!EpubLocator.startsWith("epubcfi(") || !EpubLocator.endsWith(')') || EpubLocator.contains(QChar::Null))))))return 10;
    if (Parser.isSet("rhwp-view") || RhwpEdit) {
        if ((Parser.isSet("rhwp-view") && RhwpEdit) || (Parser.isSet("rhwp-edit") && Parser.isSet("rhwp-edit-copy"))) return 10;
        if (!NewHwpx && Parser.positionalArguments().size() != 1) return 10;
        const auto Path = NewHwpx ? NewHwpxDirectory.filePath(QObject::tr("새 문서.hwpx"))
            : QFileInfo(Parser.positionalArguments().first()).absoluteFilePath();
        if (NewHwpx && (!NewHwpxDirectory.isValid() || !QFile::copy(":/templates/blank.hwpx", Path))) {
            hc::ui::MessageDialog::warning(nullptr, QObject::tr("새 문서"), QObject::tr("새 아래한글 문서를 만들지 못했습니다."));
            return 11;
        }
        const auto Format = QFileInfo(Path).suffix().toLower();
        if (Format != "hwp" && Format != "hwpx") return 10;
        if (Parser.isSet("theme") && !hc::document::DocumentThemeAdapter::SetMode(Parser.value("theme"))) return 9;
        auto Runtime = hc::document::MakeProductHwpRuntime(QCoreApplication::applicationDirPath(),
            QByteArray(HC_RHWP_APPROVED_INVENTORY_SHA256));
        if(!RhwpEdit){
            Workspace Window(hc::defaults::edit::RecoveryDirectory(true),true);
            Window.ConfigureHwp(std::move(Runtime));
            QObject::connect(&Window,&hc::document::DocumentWorkspaceWidget::StateChanged,&Window,[&Window]{
                if(Window.Snapshot()["closeRequested"].toBool())Window.close();
            });
            Window.show();
            Window.Handle({{"action","open"},{"path",Path}});return App.exec();
        }
        hc::document::HwpOpenWindow Window(Path,
            hc::defaults::edit::HwpCacheDirectory(), std::move(Runtime), {}, RhwpEdit, !NewHwpx && !Parser.isSet("rhwp-edit-copy"));
        QObject::connect(&Window, &hc::document::HwpOpenWindow::Closed, &App, &QCoreApplication::quit);
        QObject::connect(&App,&QCoreApplication::aboutToQuit,&App,[] {
            // The Ready observation has completed; all owners drain before normal quit.
            const auto Trace = hc::document::startup::Data().Snapshot();
            hc::document::startup::Write(Trace,"editor",QCoreApplication::applicationPid());
        });
        Window.show(); QTimer::singleShot(0, &Window, &hc::document::HwpOpenWindow::Open);
        return App.exec();
    }
    QObject::connect(&App, &QCoreApplication::aboutToQuit, &App, [] { hc::document::startup::Write(hc::document::startup::Data().Snapshot(), "editor", QCoreApplication::applicationPid()); });
    hc::document::startup::Scope InstanceTrace("editor.instance");
    EditorInstance Instance;
    if(!EpubEdit && !NewEpub && !Parser.isSet("find-in-files") && !Parser.isSet("document-copy")){
        const auto Canonical=[](const QString& Path){const QFileInfo Info(Path);const auto Name=Info.canonicalFilePath();return (Name.isEmpty()?Info.absoluteFilePath():Name).toCaseFolded();};
        const auto Scope=QJsonDocument(QJsonArray{Canonical(QCoreApplication::applicationFilePath()),Canonical(Config.directory_),Parser.value("theme"),Parser.value("document-appearance")}).toJson(QJsonDocument::Compact);
        const auto Started=Instance.Start(Scope,Parser.positionalArguments());
        if(Started==EditorInstance::Forwarded)return 0;
        if(Started==EditorInstance::Failed){hc::ui::MessageDialog::warning(nullptr,QObject::tr("문서 열기"),QObject::tr("기존 HyperEditor에 열기 요청을 전달하지 못했습니다. 잠시 후 다시 시도하십시오."));return 12;}
    }
    InstanceTrace.End();
    hc::document::startup::Scope SettingsTrace("editor.settings");
    // Independent installations/configurations must not consume one another's editor sessions.
    const auto Profile = QJsonDocument(QJsonArray{QDir::cleanPath(QCoreApplication::applicationFilePath()).toCaseFolded(),
        QDir::cleanPath(Config.directory_).toCaseFolded()}).toJson(QJsonDocument::Compact);
    const auto ProfileId = QString::fromLatin1(QCryptographicHash::hash(Profile, QCryptographicHash::Sha256).toHex());
    App.setProperty("documentRecoveryProfile", ProfileId);
    const auto RecoveryRoot = hc::defaults::edit::RecoveryDirectory(false);
    hc::settings::JsonStore Settings(hc::settings::UnifiedConfig::FileName(Config.directory_),
        {{"schemaVersion", 2}}, nullptr, hc::settings::JsonStore::Scope::Product);
    bool HotExitMigrationFailed = false;
    if (Settings.Object()["documentViews"].toObject()["textView"].toObject()["hotExitPolicyVersion"].toInt() != 1) {
        auto Root = Settings.Object();
        hc::settings::UnifiedConfig::Set(Root, {"documentViews", "textView", "hotExitPolicyVersion"}, 1);
        hc::settings::UnifiedConfig::Set(Root, {"documentViews", "textView", "hotExitEnabled"}, Root["documentViews"].toObject()["textView"].toObject()["hotExitEnabled"].toBool(true));
        hc::settings::UnifiedConfig::Set(Root, {"documentViews", "textView", "restoreCleanDocuments"}, Root["documentViews"].toObject()["textView"].toObject()["restoreCleanDocuments"].toBool(true));
        HotExitMigrationFailed = !Settings.Replace(Root);
    }
    const auto Appearance = Settings.Object()["appearance"].toObject();
    // Light/dark follows the product unless the editor's own setting overrides it.
    hc::document::DocumentThemeAdapter::SetMode(Appearance["documentWorkspace"].toObject()["themeMode"].toString(hc::defaults::edit::WorkspaceValue("themeMode").toString()));
    if (Parser.isSet("theme") && !hc::document::DocumentThemeAdapter::SetMode(Parser.value("theme"))) return 9;
    SettingsTrace.End();
    std::vector<std::unique_ptr<Workspace>> Windows;
    const auto OpenFiles=[&](QStringList Files, QString SessionId = {}){
    if (Files.isEmpty()) Files.append(QString{});
    Windows.reserve(Windows.size()+Files.size());
    for (const auto& File : Files) {
        hc::document::startup::Scope WindowTrace("editor.windowSetup");
        hc::document::startup::Scope ConstructorTrace("editor.workspaceConstructor");
        auto Window = std::make_unique<Workspace>(RecoveryRoot, false);
        ConstructorTrace.End();
        Window->ConfigureHwp(hc::document::MakeProductHwpRuntime(QCoreApplication::applicationDirPath(),QByteArray(HC_RHWP_APPROVED_INVENTORY_SHA256)));
        if (Appearance.contains("documentWorkspace") && !Window->ApplyChromeSettings(Appearance["documentWorkspace"].toObject()))
            Window->ShowShellError(QObject::tr("문서 표시 설정을 적용하지 못하여 기본 표시를 사용합니다."));
        if(!ParentAppearance.isEmpty()) hc::document::epubui::ApplyAppearance(ParentAppearance);
        auto* Current = Window.get();
        if (HotExitMigrationFailed) Current->ShowShellError(QObject::tr("HotExit 설정을 저장하지 못했습니다. 기존 종료 동작을 유지합니다."));
        QObject::connect(Current, &hc::document::DocumentWorkspaceWidget::ChromeSettingsChanged, &App,
            [Current, &Settings](const QJsonObject& Chrome) {
                auto Next = Settings.Object()["appearance"].toObject(); Next["documentWorkspace"] = Chrome;
                auto Root = Settings.Object(); Root["appearance"] = Next;
                if (!Settings.Replace(Root)) Current->ShowShellError(QObject::tr("표시 설정을 저장하지 못했습니다."));
                else Current->ShowShellError({});
            });
        if (hc::document::startup::Enabled()) {
            const auto Ready = std::make_shared<bool>(false);
            QObject::connect(Current, &hc::document::DocumentWorkspaceWidget::StateChanged, Current, [Current, Ready] {
                if (*Ready || !Current->IsIdle()) return;
                for (const auto& Item : Current->Snapshot()["documents"].toArray()) {
                    const auto Record = Item.toObject();
                    if (!Record["loading"].toBool() && Record["error"].toString().isEmpty()
                        && Record["view"].toObject()["viewId"] == "text") {
                        *Ready = true;
                        hc::document::startup::Data().Aggregate("editor.textReady", hc::document::startup::Data().Now());
                        break;
                    }
                }
            });
        }
        const auto EditPending=std::make_shared<bool>(EpubEdit);
        if(Parser.isSet("find-in-files")) {
            auto* SearchWait=new QTimer(Current); SearchWait->setInterval(50);
            QObject::connect(SearchWait,&QTimer::timeout,Current,[Current,SearchWait,SearchContext] {
                const auto Documents=Current->Snapshot()["documents"].toArray(); if(Documents.isEmpty())return;
                const auto Record=Documents.first().toObject(); if(Record["loading"].toBool() || !Current->IsIdle())return;
                SearchWait->stop();SearchWait->deleteLater();
                if(Record["error"].toString().isEmpty())Current->OpenSearchContext(SearchContext);
            }); SearchWait->start();
        }
        QObject::connect(Current, &hc::document::DocumentWorkspaceWidget::StateChanged, Current, [Current,EditPending,EpubLocator,EpubChapter] {
            const auto Documents = Current->Snapshot()["documents"].toArray();
            for(const auto& Item : Documents) {
                const auto Record = Item.toObject();
                if(Record["view"].toObject()["viewId"] != "epub") continue;
                if(*EditPending && !Record["loading"].toBool() && Record["error"].toString().isEmpty()) {
                    *EditPending=false;const auto Id=Record["documentId"].toString();
                    QTimer::singleShot(0,Current,[Current,Id,EpubLocator,EpubChapter]{
                        const auto Result=Current->Handle({{"action","epub.edit.begin"},{"documentId",Id},{"locator",EpubLocator},{"chapter",EpubChapter}});
                        if(Result.contains("error"))Current->ShowShellError(QObject::tr("EPUB 편집 모드를 열지 못했습니다: %1").arg(Result["error"].toString()));
                    });
                }
                return;
            }
        });
        Current->closed_=[&App,&Windows,Current]{QTimer::singleShot(0,&App,[&Windows,Current]{
            std::erase_if(Windows,[Current](const auto& Window){return Window.get()==Current;});
        });};
        WindowTrace.End();
        hc::document::startup::Scope ShowTrace("editor.show");
        // show() activates a newly created top-level window; avoid redundant native activation.
        Current->show();
        ShowTrace.End();
        hc::document::startup::Scope OpenTrace("editor.openRequest");
        if (!SessionId.isEmpty() && !Current->RestoreHotExit(SessionId)) continue;
        if (!File.isEmpty() && Current->Handle({{"action", "open"}, {"path", QFileInfo(File).absoluteFilePath()},{"copyOnly",Parser.isSet("document-copy")}}).contains("error"))
            Current->ShowShellError(QObject::tr("문서 한도 때문에 파일을 열지 못했습니다: %1").arg(File));
        Windows.push_back(std::move(Window));
    }
    };
    Instance.SetOpenHandler([&](const QStringList& Files) { OpenFiles(Files); });
    // Show a shell first. Restoration itself reads snapshots off the GUI thread.
    QTimer::singleShot(0, &App, [&] {
        QSet<QString> RestoredPaths; bool Restored = false;
        const auto Text = Settings.Object()["documentViews"].toObject()["textView"].toObject();
        if (!EpubEdit && !NewEpub && !Parser.isSet("document-copy") && !Parser.isSet("find-in-files") && Text["hotExitEnabled"].toBool(true)) {
            hc::document::DocumentRecoveryStore Store(RecoveryRoot);
            for (const auto& Id : Store.Sessions()) {
                auto Session = Store.ReadSession(Id); if (!Session.HasValue() || Session.Value()["profileId"].toString() != ProfileId || Session.Value()["documents"].toArray().isEmpty()) continue;
                const auto Before = Windows.size(); OpenFiles({}, Id);
                if (Windows.size() > Before) {
                    Restored = true;
                    for (const auto& Item : Session.Value()["documents"].toArray()) {
                        const auto Path = Item.toObject()["path"].toString();
                        if (!Path.isEmpty()) RestoredPaths.insert(QDir::cleanPath(QFileInfo(Path).absoluteFilePath()).toCaseFolded());
                    }
                }
            }
        }
        QStringList Files; for (const auto& File : Parser.positionalArguments())
            if (!RestoredPaths.contains(QDir::cleanPath(QFileInfo(File).absoluteFilePath()).toCaseFolded())) Files.append(File);
        if (!Files.isEmpty() || !Restored) OpenFiles(Files);
        // Cancelling the dialog of a --new-epub window with nothing open closes that window.
        if (NewEpub && !Windows.empty()) Windows.back()->ShowNewEpubDialog(true);
    });
    hc::document::startup::Data().EndNamed("editor.entryToTitle");
    QTimer::singleShot(0, &App, [] { hc::document::startup::Data().Aggregate("editor.eventLoopReached", hc::document::startup::Data().Now()); });
    return App.exec();
}
