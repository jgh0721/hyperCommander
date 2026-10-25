#include "UniqueLibs/wcxStorage.hpp"
#include "UniqueLibs/archiveExtractJob.hpp"
#include "UniqueLibs/qtJobJournal.hpp"
#include "UniqueLibs/operationMetrics.hpp"
#include "UniqueLibs/fileIconService.hpp"
#include "HyperCommander/UIs/fileColorRulesPage.hpp"
#include "UniqueLibs/settings/singleInstance.hpp"
#include "UniqueLibs/columnAutoSelection.hpp"
#include "HyperCommander/UIs/fileToolsDialogs.hpp"
#include "HyperCommander/UIs/dialogShell.hpp"
#include "HyperCommander/UIs/itemSelectionController.hpp"
#include "UniqueLibs/itemSelection.hpp"
#include <QRadioButton>
#include <ShlObj.h>
#include <QInputMethodEvent>
#include <imm.h>
#include "UniqueLibs/quickFindSettings.hpp"
#include "everythingStreamFixture.hpp"
#include "UniqueLibs/quickFindHighlight.hpp"
#include <QAccessible>
#include <QScrollArea>
#include <QScrollBar>
#include "HyperCommander/UIs/permissionDialogs.hpp"
#include <fmwidgets/RenameComparisonView.h>
#include <fmwidgets/SegmentedControl.h>
#include <fmwidgets/BreadcrumbBar.h>
#include "HyperCommander/UIs/panelPathBar.hpp"
#include <fmwidgets/ProgressBar.h>
#include <fmwidgets/PanelTabStrip.h>
#include <fmwidgets/CommandLine.h>
#include <fmwidgets/WindowChrome.h>
#include <fmwidgets/FunctionKeyBar.h>
#include <fmwidgets/FindBox.h>
#include <fmfilelist/ListPresentation.h>
#include <fmfilelist/ThumbnailView.h>
#include <fmfilelist/ThumbnailProvider.h>
#include <fmfilelist/FileRoles.h>
#include "HyperCommander/UIs/Components/thumbnailModel.hpp"
#include "HyperCommander/UIs/thumbnailSettingsPage.hpp"
#include <fmwidgets/DialogWidgets.h>
#include <fmwidgets/Button.h>
#include "stdafx.h"
#include <QDrag>
#include "UniqueLibs/shellDriveSort.hpp"
#include <QMimeData>
#include <QtTest>
#include <mutex>
#include <set>
#include <QCryptographicHash>
#include "cmnHCUtils.hpp"
#include "rhwpOpenTimingHarness.hpp"
#include <QtConcurrent/QtConcurrentRun>
#include "HyperCommander.hpp"
#include "UIs/dlgMain.hpp"
#include "UIs/dlgOpts.hpp"
#include "UniqueLibs/passwords/passwordUi.hpp"
#include "UIs/dlgFavoriteDir.hpp"
#include "solFavoriteDirMgr.hpp"
#include "UIs/menuBarSettingsPage.hpp"
#include "UIs/themeSettingsPage.hpp"
#include "UIs/keyboardSettingsPage.hpp"
#include "UIs/legacySettingsPage.hpp"
#include "UIs/cmpPanel.hpp"
#include "memoryStorage.hpp"
#include "commandMgr.hpp"
#include "qtitanColumnAdapter.hpp"
#include "columnMgr.hpp"
#include "qtColumnProfileEditor.hpp"
#include "columnPluginFields.hpp"
#include "columnNativeFields.hpp"
#include "fileJobUi.hpp"
#include <QTreeWidget>
#include <fmwidgets/TransferGraph.h>
#include <fmwidgets/DialogHeader.h>
#include <fmstyle/ThemeManager.h>
#include "commonLibs/operations/itemOutcomeStore.hpp"
#include "nativeWritableStorage.hpp"
#include "nativeMaterializer.hpp"
#include "storageProviders.hpp"
#include "wcxMutation.hpp"
#include "HyperCommanderBuiltInPlugin/IsoImage/builtinIso.hpp"
#include "HyperCommanderBuiltInPlugin/IsoImage/isoRewrite.hpp"
#include "HyperCommanderBuiltInPlugin/SevenZip/builtinSevenZip.hpp"
#include "HyperCommanderBuiltInPlugin/AlzEgg/builtinUnEgg.hpp"

#include "qtViewerSurface.hpp"
#include <QPdfWriter>
#include <QPainter>
#include <QtnGridEditors.h>
#include <fpdfview.h>
#include <fpdf_edit.h>
#include "qtViewerHost.hpp"
#include "qtPluginViewerSurface.hpp"
#include "UniqueLibs/document/documentOpenController.hpp"
#include "qtPluginHostSettings.hpp"
#include "qtPluginManagementPage.hpp"
#include "pluginHost/manifestFile.hpp"
#include <QtitanGrid.h>
#include <iostream>
#include <QTreeWidget>
#include "qtAutomation.hpp"
#include "qtWorkspaceRestore.hpp"
#include <QInputDialog>
#include <psapi.h>
#include <AclAPI.h>
#include "driveProbeFixture.hpp"
#include "qtitanRegression.hpp"
#include "filePanelGroupingUi.hpp"
#include "inlineRenameGeometryUi.hpp"
#include "colorSchemeMgr.hpp"
#include <fmstyle/ThemeManager.h>
#include <fmstyle/WatercolorStyle.h>
#include <QProxyStyle>
#ifdef _DEBUG
#include <crtdbg.h>
#endif

// Native timers continue to run inside TrackPopupMenu's Windows message loop.
// Close only this test thread's menu; never invoke a shell verb during UI tests.
class ShellMenuProbe final
{
public:
    ShellMenuProbe() { active_ = this; hook_ = SetWindowsHookExW(WH_CALLWNDPROC, observe, nullptr, GetCurrentThreadId()); timer_ = SetTimer(nullptr, 0, 40, tick); }
    ~ShellMenuProbe() { KillTimer(nullptr, timer_); UnhookWindowsHookEx(hook_); active_ = nullptr; }
    bool opened_ = false;
    int items_ = 0;
private:
    static LRESULT CALLBACK observe(int Code, WPARAM WParam, LPARAM LParam)
    {
        if (Code >= 0 && active_) {
            const auto* Msg = reinterpret_cast<const CWPSTRUCT*>(LParam);
            if (Msg->message == WM_INITMENUPOPUP) active_->menu_ = reinterpret_cast<HMENU>(Msg->wParam);
        }
        return CallNextHookEx(nullptr, Code, WParam, LParam);
    }
    static void CALLBACK tick(HWND, UINT, UINT_PTR, DWORD)
    {
        if (!active_) return;
        GUITHREADINFO Info{sizeof(Info)};
        if (GetGUIThreadInfo(GetCurrentThreadId(), &Info) && (Info.flags & GUI_INMENUMODE)) {
            active_->opened_ = true; active_->items_ = GetMenuItemCount(active_->menu_); EndMenu();
        }
    }
    inline static thread_local ShellMenuProbe* active_ = nullptr;
    UINT_PTR timer_ = 0;
    HHOOK hook_ = nullptr;
    HMENU menu_ = nullptr;
};

// OLE does not continuously pump Qt timers while the physical mouse is idle.
class NativeDragProbe final
{
public:
    explicit NativeDragProbe(QObject* Owner) : owner_(Owner)
    { active_ = this; timer_ = SetTimer(nullptr, 0, 20, tick); }
    ~NativeDragProbe() { KillTimer(nullptr, timer_); active_ = nullptr; }
    bool seen_ = false;
    QStringList paths_;
private:
    static void CALLBACK tick(HWND, UINT, UINT_PTR, DWORD)
    {
        if (!active_) return;
        if (const auto* Drag = active_->owner_->findChild<QDrag*>()) {
            if (!active_->seen_) {
                active_->seen_ = true;
                for (const auto& Url : Drag->mimeData()->urls()) active_->paths_.append(Url.toLocalFile());
            }
            QDrag::cancel();
            if (const auto Capture = GetCapture()) PostMessageW(Capture, WM_MOUSEMOVE, 0, 0);
            PostThreadMessageW(GetCurrentThreadId(), WM_MOUSEMOVE, 0, 0);
        }
    }
    inline static thread_local NativeDragProbe* active_ = nullptr;
    QObject* owner_;
    UINT_PTR timer_ = 0;
};

class GatedProvider : public hc::storage::IStorageProvider
{
public:
    std::promise<void> release_;
    std::shared_future<void> gate_ = release_.get_future().share();
    std::atomic<bool> started_{false};
    bool fail_ = false;
    hc::storage::test::MemoryProvider inner_;
    hc::storage::ProviderId Id() const override { return inner_.Id(); }
    hc::storage::Task<hc::storage::SupportDecision> Supports(hc::storage::Operation Op, hc::storage::OperationContext Ctx) override { return inner_.Supports(Op, Ctx); }
    hc::storage::Task<std::unique_ptr<hc::storage::IEntryCursor>> Enumerate(hc::storage::Locator At, hc::storage::OperationContext Ctx) override
    {
        started_ = true;
        gate_.wait();
        if (fail_) return hc::storage::Ready<std::unique_ptr<hc::storage::IEntryCursor>>(hc::storage::Error{hc::storage::ErrorCategory::Unavailable, "test-listing-failed"});
        return inner_.Enumerate(At, Ctx);
    }
    hc::storage::Task<hc::storage::EntrySnapshot> Stat(hc::storage::ResourceId Id, hc::storage::OperationContext Ctx) override { return inner_.Stat(Id, Ctx); }
    hc::storage::Task<std::shared_ptr<hc::storage::IReadStream>> OpenRead(hc::storage::ResourceId Id, hc::storage::OperationContext Ctx) override { return inner_.OpenRead(Id, Ctx); }
};
class GatedFs : public nsHC::CFileSystemT
{
public:
    explicit GatedFs(std::shared_ptr<GatedProvider> Provider) : CFileSystemT("C:"), provider_(std::move(Provider)) {}
    std::shared_ptr<hc::storage::IStorageProvider> ReadProvider() override { return provider_; }
    hc::storage::Locator ReadLocation(const QString&) const override { return provider_->inner_.Root(); }
private:
    std::shared_ptr<GatedProvider> provider_;
};

static QElapsedTimer StartupTimer;


static void SetPluginField(hc::settings::Repository& Settings, const QString& Format, int Index, const QString& Field, const QJsonValue& Value)
{
    auto Items = Settings.Plugins(Format); while (Items.size() <= Index) Items.append(QJsonObject{});
    auto Record = Items[Index].toObject(); Record[Field] = Value; Items[Index] = Record; Settings.SetPlugins(Format, Items);
}
class R9Manifest final : public hc::storage::ISelectionManifest {
public:
    std::vector<hc::storage::EntrySnapshot> entries_;
    std::shared_ptr<hc::storage::IStorageProvider> provider_;
    mutable std::atomic<std::size_t> maxRead_{0};
    std::uint64_t Count() const override { return entries_.size(); }
    std::shared_ptr<hc::storage::IStorageProvider> Provider() const override { return provider_; }
    hc::storage::Result<hc::storage::EntryPage> Read(std::uint64_t Offset,std::size_t MaxItems,const hc::storage::OperationContext& Context) const override {
        if (auto Error = Context.Check(provider_->Id())) return *Error;
        maxRead_ = (std::max)(maxRead_.load(),MaxItems); hc::storage::EntryPage Page;
        const auto End = (std::min)(entries_.size(),std::size_t(Offset)+MaxItems);
        for (auto I = std::size_t(Offset); I < End; ++I) Page.entries_.push_back(entries_[I]); Page.end_ = End == entries_.size(); return Page;
    }
};
static bool SaveR9Dialog(QWidget& Dialog,const QString& Name) {
    const auto Out = qEnvironmentVariable("HC_UI_ARTIFACT_DIR"); if (Out.isEmpty()) return true; QDir().mkpath(Out);
    auto& Theme = fm::style::ThemeManager::instance(); const auto Before = Theme.scheme();
    const auto Restore = qScopeGuard([&] { Theme.setScheme(Before); });
    for (const bool Dark : {false,true}) {
        Theme.setScheme(Dark ? fm::style::ThemeManager::Scheme::Dark : fm::style::ThemeManager::Scheme::Light); QTest::qWait(100);
        if (!Dialog.grab().save(QDir(Out).filePath(Name + (Dark ? "-dark-" : "-light-") + QString::number(Dialog.devicePixelRatioF()) + ".png"))) return false;
    }
    return true;
}
class SmbTests : public QObject
{
    Q_OBJECT
private:
    bool legacyOpen(CFSModel* Model, const QModelIndex& Index) {
        const auto Entry = Model->GetFileInfo(Index); auto* Host = static_cast<QtViewerHost*>(main_->findChild<QObject*>("hc.viewer.host"));
        return Host && Entry && Entry->storageEntry_ && Host->OpenFull(std::make_shared<hc::storage::ProviderContentSource>(Model->SnapshotProvider(), *Entry->storageEntry_));
    }
private slots:
    void filePanelGroupingUi() { hc::tests::FilePanelGroupingUi(main_, panels_.front()); }
    void filePanelGroupingInputUi() { hc::tests::FilePanelGroupingInputUi(main_, panels_.front()); }
    void filePanelGroupingReentryUi() { hc::tests::FilePanelGroupingReentryUi(main_, panels_.front()); }
    void filePanelGroupingSearchUi() { hc::tests::FilePanelGroupingSearchUi(main_, panels_.front()); }
    void filePanelGroupingLargeUi() { hc::tests::FilePanelGroupingLargeUi(main_, panels_.front()); }
    void initTestCase()
    {
        server_ = qEnvironmentVariable("HC_SMB_SERVER", "DEVTESTER-QTS");
        share_ = qEnvironmentVariable("HC_SMB_SHARE", "Public");
        QVERIFY(!server_.contains('\\') && !share_.contains('\\'));
        const auto LocalRoot = qEnvironmentVariable("HC_UI_LOCAL_ROOT");
        localOnly_ = !LocalRoot.isEmpty();
        unc_ = localOnly_ ? LocalRoot : "\\\\" + server_ + "\\" + share_;
        QVERIFY2(QDir(unc_).exists(), localOnly_ ? "Local UI test root must exist" : "Live SMB share must be accessible before running this suite");
        if (localOnly_)
            qInfo(qEnvironmentVariableIsSet("HC_TEST_MAPPED_IO")
                ? "Mapped-drive I/O mode: explicit mapping required; local folder used for recovery"
                : "Local UI mode: no SMB I/O coverage; only explicitly allowed scenarios may run");
        for (auto* Window : QApplication::topLevelWidgets())
            if (auto* Main = qobject_cast<QMainUI*>(Window))
                main_ = Main;
        QVERIFY(main_);
        const auto ExpectedDpr = qEnvironmentVariable("HC_UI_EXPECT_DPR");
        if (!ExpectedDpr.isEmpty()) QCOMPARE(main_->devicePixelRatioF(), ExpectedDpr.toDouble());
        qInfo() << "Product UI DPR:" << main_->devicePixelRatioF();
        panels_ = main_->findChildren<CmpPanel*>();
        QCOMPARE(panels_.size(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(model(panels_.front())->GetRoot(), 5000);
        for (auto* Panel : panels_) QTRY_VERIFY_WITH_TIMEOUT(!model(Panel)->IsLoading(), 15000);
    }

    void init()
    {
        if (localOnly_)
        {
            const QStringList Allowed{"fileCompareUi", "archiveNavigationPositionUi", "navigationProgressUi", "navigationEnterRepeatUi","fileDeleteCursorUi", "itemSelectionUi", "driveRootNavigationUi", "folderSizesUi", "renameCancellationFocusUi", "hiddenViewerPreparationKeepsFocusUi", "dialogFocusReturnUi", "shellMenuUi", "inlineRenameClicksUi", "inlineRenameUi", "inlineRenameGeometryUi", "pluginApplicationSelectionUi", "rhwpDownloadsTimingUi", "rhwpF4ProductUi", "rhwpF3ProductUi", "documentProductSavesUi", "documentCommandsUi", "epubProductKeys", "epubStartupFailureUi", "r23FileWritesUi", "r23ArchiveBenchmark", "r22LaboratoryUi", "r22ArchiveBenchmark", "r21TransferProgressUi", "r21ArchiveBenchmark", "r20LaboratoryUi", "r20ArchiveBenchmark", "r19ArchiveBenchmark", "r18ManyFilesExtractUi", "r18LargeExtractUi", "r16ArchiveCommandsUi", "r15RenameQueueUi", "r15RenameRestartUi", "r14ThemeConsistencyUi", "r14ThemeRestartUi", "r14SettingsPolishUi", "r13SettingsOptionsUi", "r13AutomaticColumnsUi", "r13SingleInstanceUi", "r13RestartUi", "r12ReferenceMainUi", "r12RestartUi", "r11ResponsiveJobsUi", "r10PermissionChoicesUi", "r9FileMasksUi", "r9LargeRenameUi", "r9NewFileUi", "r8CopyDefaultsUi", "r8CopyCommandsUi", "r7JobHistoryRestartUi", "r7QueueAndRetryUi", "r6FileDialogsUi", "r6FileOperationsUi", "r6RecentRestartUi", "r5SettingsUi", "r5RestartUi", "menuBarSettingsUi", "thumbnailProviderUi", "thumbnailViewUi", "thumbnailSettingsUi", "thumbnailRestartUi", "listPresentationUi", "mainPanelChromeUi", "permissionsUi", "fileToolsUi", "fileColorSettingsUi", "fileColorAppearanceUi", "fileColorPreviewUi", "fileColorFinalReviewUi", "fileColorConditionsUi", "fileColorPatternModesUi", "fileColorFirstPaintUi", "fileColorLargeFolderUi", "removedLegacyAppearanceUi", "backgroundJobManagerUi", "builtinNestedArchives", "sevenZipCreateUi", "builtinUnEggNavigation", "builtinSevenZipNavigation", "fileJobResultPagesUi", "closingWindowCancelsWaitingFileJob", "activePanelAppearanceUi", "displayAndNavigationSettingsUi", "directoryHotListPlacementUi", "navigationRefreshSettingsUi", "comboAndListPreferencesUi", "watercolorSettingsUi", "watercolorGridStateUi", "firstRunAppearanceUi", "appearanceInteractionUi", "columnWdxUi", "columnLuaUi", "columnsLargeListingUi", "columnWorkspaceSelectionUi", "columnPreviewAndScopesUi", "columnTemplatesAndAsyncUi", "listLayoutSettingsUi", "listLayoutRestartUi", "jsonSettingsUi", "passwordAndNestedArchivesUi", "qtitanCacheAndSortUi", "qtitanFileManagerUi", "columnMetadataTextUi", "columnFitToPaneUi", "columnWidthLayoutAndSettings", "columnResizeVisibilityAndScrollbars", "automationCommandsUi", "scopedSearchUi", "workspaceBootUi", "workspaceRestoreUi", "workspaceWatchAndMetadataUi", "protectedParentNavigationUi", "isoPluginUi", "dopusPluginsUi", "dataPluginsUi", "networkDriveStartup", "archiveCommandsAndBatchUi", "archiveCrossContainerMovesUi", "viewerCommandsAndPreviewUi", "wcxHostSettingsPersistence", "wcxHostApprovalUi", "configuredHostNavigation", "viewerLifetimeSoak", "wlxViewerCommandsUi"};
            QVERIFY2(Allowed.contains(QString::fromLatin1(QTest::currentTestFunction()))
                || QString::fromLatin1(QTest::currentTestFunction()) == "passwordManagerSettingsUi"
                || QString::fromLatin1(QTest::currentTestFunction()) == "driveListOrderUi"
                || QString::fromLatin1(QTest::currentTestFunction()) == "externalFileDragUi"
                || QString::fromLatin1(QTest::currentTestFunction()) == "backgroundMetadataNavigationUi"
                || QString::fromLatin1(QTest::currentTestFunction()) == "fileIconRefreshUi"
                || QString::fromLatin1(QTest::currentTestFunction()) == "nativeParallelSelectionUi"
                || QString::fromLatin1(QTest::currentTestFunction()) == "panelBreadcrumbUi"
                || QString::fromLatin1(QTest::currentTestFunction()) == "panelBreadcrumbNavigationUi"
                || (QStringList{"legacySettingsDraftUi", "legacySettingsApplyCancelUi", "legacySettingsLayoutUi"}.contains(QString::fromLatin1(QTest::currentTestFunction())))
                || QString::fromLatin1(QTest::currentTestFunction()).startsWith("todoBug")
                || QString::fromLatin1(QTest::currentTestFunction()) == "everythingPanelStreamingUi"
                || QString::fromLatin1(QTest::currentTestFunction()).startsWith("filePanelGrouping"), "Local UI mode cannot execute an SMB scenario");
        }
    }



    void fileCompareUi()
    {
        auto& Settings = hc::settings::AppSettings(); const auto Saved = Settings.Object();
        const auto Workspace = main_->CaptureWorkspace();
        const auto Restore = qScopeGuard([&] { Settings.Replace(Saved); TyStCommandMgr::GetInstance()->Refresh(); main_->RestoreWorkspace(Workspace); });
        auto General = Settings.Read(hc::settings::Section::General).toObject(); General["useBuiltInShortcuts"] = true;
        QVERIFY(Settings.Put(hc::settings::Section::General, General)); QVERIFY(Settings.Put(hc::settings::Section::Shortcuts, QJsonObject{}));
        TyStCommandMgr::GetInstance()->Refresh();
        QTemporaryDir Left, Right;
        for (const auto& Name : {"a file.txt", "b.txt", "c.txt", "d.txt"}) { QFile File(Left.filePath(Name)); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("left"); }
        { QFile File(Right.filePath("right file.txt")); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("right"); }
        QVERIFY(QDir(Left.path()).mkdir("folderA")); QVERIFY(QDir(Left.path()).mkdir("folderB"));
        auto* L = main_->findChild<CmpPanel*>("cmpLeftPanel"); auto* R = main_->findChild<CmpPanel*>("cmpRightPanel"); QVERIFY(L && R);
        L->ChangeCurrentPath(Left.path(), {}); R->ChangeCurrentPath(Right.path(), {});
        QTRY_VERIFY(!model(L)->IsLoading() && !model(R)->IsLoading());
        QTRY_COMPARE(model(L)->rowCount(), 7); QTRY_COMPARE(model(R)->rowCount(), 2);
        const auto Select = [&](CmpPanel* Panel, const QStringList& Names, bool Mark = true) {
            auto* View = qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex()));
            auto* Grid = View->BaseView(); Grid->deselectAll(); QModelIndex First;
            for (const auto& Name : Names) for (int Row = 0; Row < Grid->getRowCount(); ++Row) {
                auto Index = Grid->getRow(Row).modelIndex(0);
                while (const auto* Proxy = qobject_cast<const QAbstractProxyModel*>(Index.model())) Index = Proxy->mapToSource(Index);
                if (!Index.isValid() || model(Panel)->GetName(Index) != Name) continue;
                if (!First.isValid()) { First = Index; Grid->setFocusedRowIndex(Row); }
                if (Mark) Grid->selectRow(Row, Qtitan::Select);
            }
            return First;
        };
        auto* Bridge = TyStCommandMgr::GetInstance()->Bridge(); QVERIFY(Bridge->Action("hc.file.compare"));
        {
            hc::ui::KeyboardSettingsPage Keys;
            auto* Selector = static_cast<hc::ui::CommandSelector*>(Keys.findChild<QComboBox*>("hc.keys.command"));
            QVERIFY(Selector && Selector->Commands().contains("hc.file.compare"));
            auto* Search = Keys.findChild<QLineEdit*>("hc.keys.search");
            auto* Table = Keys.findChild<QTableWidget*>("hc.keys.commands");
            Search->setText("hc.file.compare"); int Visible = 0;
            for (int Row = 0; Row < Table->rowCount(); ++Row) if (!Table->isRowHidden(Row)) {
                ++Visible; QCOMPARE(Table->item(Row, 0)->data(Qt::UserRole).toString(), QString("hc.file.compare"));
                QCOMPARE(QKeySequence(Table->item(Row, 1)->text()), QKeySequence("Ctrl+O"));
                QCOMPARE(QKeySequence(Table->item(Row, 2)->text()), QKeySequence("Ctrl+O"));
            }
            QCOMPARE(Visible, 1);
        }
        auto Index = Select(L, {"a file.txt", "b.txt"}); auto Args = Bridge->CompareArguments(Index); QVERIFY(Args.HasValue());
        QCOMPARE(Args.Value().size(), 6); QCOMPARE(Args.Value()[1], "compare");
        QVERIFY(Args.Value().contains(QDir::toNativeSeparators(Left.filePath("a file.txt"))));
        Index = Select(L, {"a file.txt", "b.txt", "c.txt"}); Args = Bridge->CompareArguments(Index); QVERIFY(Args.HasValue()); QCOMPARE(Args.Value().size(), 8); QCOMPARE(Args.Value()[6], "-file3");
        Index = Select(L, {"a file.txt", "b.txt", "c.txt", "d.txt"}); QVERIFY(!Bridge->CompareArguments(Index).HasValue());
        Index = Select(L, {"folderA", "folderB"}); Args = Bridge->CompareArguments(Index); QVERIFY(Args.HasValue()); QCOMPARE(Args.Value()[1], "directory");
        Index = Select(L, {"a file.txt", "folderA"}); QVERIFY(!Bridge->CompareArguments(Index).HasValue());
        Index = Select(L, {"a file.txt"}, false); auto OtherIndex = Select(R, {"right file.txt"}, false);
        Args = Bridge->CompareArguments(Index); QVERIFY(Args.HasValue());
        QCOMPARE(Args.Value()[3], QDir::toNativeSeparators(Left.filePath("a file.txt"))); QCOMPARE(Args.Value()[5], QDir::toNativeSeparators(Right.filePath("right file.txt")));
        const auto Reverse = Bridge->CompareArguments(OtherIndex); QVERIFY(Reverse.HasValue()); QCOMPARE(Reverse.Value(), Args.Value());
        Select(L, {"a file.txt", "b.txt"}); QVERIFY(!Bridge->CompareArguments(OtherIndex).HasValue());
        Index = Select(L, {"a file.txt"}); Select(R, {"right file.txt"}); QVERIFY(Bridge->CompareArguments(Index).HasValue());
        Select(R, {".."}, false); QVERIFY(!Bridge->CompareArguments(Index).HasValue());
        R->ChangeCurrentPath(Left.path(), {}); QTRY_VERIFY(!model(R)->IsLoading()); QTRY_COMPARE(model(R)->rowCount(), 7);
        Select(R, {"a file.txt"}); const auto Same = Bridge->CompareArguments(Index); QVERIFY(!Same.HasValue()); QCOMPARE(Same.Failure().code_, std::string("compare-same-path"));
        const auto CompareWindows = [] {
            struct Scan { QString executable_; QList<HWND> windows_; } State{QDir::toNativeSeparators(QCoreApplication::applicationDirPath() + "/HyperCompare.exe"), {}};
            EnumWindows([](HWND Window, LPARAM Data) -> BOOL {
                auto& ScanState = *reinterpret_cast<Scan*>(Data); DWORD Pid = 0; GetWindowThreadProcessId(Window, &Pid);
                const auto Process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, Pid);
                if (Process) {
                    wchar_t Path[32768]{}; DWORD Length = 32768;
                    if (QueryFullProcessImageNameW(Process, 0, Path, &Length) && ScanState.executable_.compare(QString::fromWCharArray(Path, int(Length)), Qt::CaseInsensitive) == 0 && IsWindowVisible(Window))
                        ScanState.windows_.append(Window);
                    CloseHandle(Process);
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&State));
            return State.windows_;
        };
        R->ChangeCurrentPath(Right.path(), {}); QTRY_VERIFY(!model(R)->IsLoading()); QTRY_COMPARE(model(R)->rowCount(), 2);
        // Exercise both selection forms through the real file-panel key event.
        for (bool BothPanels : {false, true}) {
            const auto Before = CompareWindows();
            const auto CloseCompare = qScopeGuard([&] { for (auto Window : CompareWindows()) if (!Before.contains(Window)) PostMessageW(Window, WM_CLOSE, 0, 0); });
            Select(L, BothPanels ? QStringList{"a file.txt"} : QStringList{"a file.txt", "b.txt"});
            if (BothPanels) Select(R, {"right file.txt"});
            auto* View = qobject_cast<CGridView*>(L->GetFocusView(L->CurrentTabIndex())); View->EnsureKeyboardFocusOnView();
            QTRY_VERIFY(Bridge->Action("hc.file.compare")->isEnabled());
            QTRY_VERIFY(QApplication::focusWidget() && View->isAncestorOf(QApplication::focusWidget()));
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_O, Qt::ControlModifier);
            const auto Launched = [&] {
                for (auto Window : CompareWindows()) if (!Before.contains(Window)) {
                    wchar_t Title[1024]{}; GetWindowTextW(Window, Title, 1024);
                    if (QString::fromWCharArray(Title).contains("HyperCompare")) return true;
                }
                return false;
            };
            QTRY_VERIFY_WITH_TIMEOUT(Launched(), 15000);
        }
    }

    void fileDeleteCursorUi()
    {
        using namespace hc::operations;
        const auto Workspace = main_->CaptureWorkspace();
        const auto Restore = qScopeGuard([&] { main_->RestoreWorkspace(Workspace); });
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        for (int I = 0; I < 120; ++I) {
            QFile File(Directory.filePath(QString("entry%1.txt").arg(I, 3, 10, QChar('0'))));
            QVERIFY(File.open(QIODevice::WriteOnly)); File.write("cursor fixture");
        }
        auto* Panel = panels_.front(); Panel->SetSearch({});
        Panel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {}); Panel->sig_NotifyPanelActivated();
        auto* Model = static_cast<CFSModel*>(model(Panel));
        auto* View = static_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex()));
        QTRY_VERIFY(!Model->IsLoading()); QTRY_COMPARE(Model->rowCount(), 121);
        QVERIFY(View->ApplyColumnProfile(hc::columns::DefaultProfile())); QVERIFY(View->SetListMode("one-line"));
        auto* Grid = View->BaseView();
        const auto RowFor = [&](const QString& Name) {
            for (int I = 0; I < Grid->getRowCount(); ++I) {
                auto Index = Grid->getRow(I).modelIndex(0);
                while (const auto* Proxy = qobject_cast<const QAbstractProxyModel*>(Index.model())) Index = Proxy->mapToSource(Index);
                if (Index.isValid() && Model->GetName(Index) == Name) return I;
            }
            return -1;
        };
        const auto FocusName = [&] {
            auto Index = Grid->focusedRow().modelIndex(0);
            while (const auto* Proxy = qobject_cast<const QAbstractProxyModel*>(Index.model())) Index = Proxy->mapToSource(Index);
            return Index.isValid() ? Model->GetName(Index) : QString{};
        };
        auto* Bridge = TyStCommandMgr::GetInstance()->Bridge();
        const auto Delete = [&](const QString& Focus, const QStringList& Selected) {
            Grid->deselectAll(); Grid->setFocusedRowIndex(RowFor(Focus));
            for (const auto& Name : Selected) Grid->selectRow(RowFor(Name), Qtitan::Select);
            View->EnsureKeyboardFocusOnView();
            return Bridge->Dispatch("hc.file.delete", {{"deleteMode", "permanent"}, {"confirmed", true}}, "cursor-test");
        };
        auto Single = Delete("entry060.txt", {});
        QTRY_VERIFY(Single.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        const auto SingleJob = Single.get(); QVERIFY(SingleJob.HasValue());
        QTRY_VERIFY_WITH_TIMEOUT(IsTerminal(Bridge->FileJobs()->Scheduler().Snapshot(SingleJob.Value().jobId_).Value().state_), 10000);
        QCOMPARE(Bridge->FileJobs()->Scheduler().Snapshot(SingleJob.Value().jobId_).Value().state_, JobState::Completed);
        QTRY_VERIFY(!Model->IsLoading() && Model->rowCount() == 120);
        QTRY_COMPARE(FocusName(), QString("entry061.txt"));
        QVERIFY(SaveR9Dialog(*main_, "delete-cursor-middle"));
        auto Multiple = Delete("entry061.txt", {"entry010.txt", "entry061.txt", "entry062.txt"});
        QTRY_VERIFY(Multiple.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        const auto MultipleJob = Multiple.get(); QVERIFY(MultipleJob.HasValue());
        QTRY_VERIFY_WITH_TIMEOUT(IsTerminal(Bridge->FileJobs()->Scheduler().Snapshot(MultipleJob.Value().jobId_).Value().state_), 10000);
        QTRY_VERIFY(!Model->IsLoading() && Model->rowCount() == 117);
        QTRY_COMPARE(FocusName(), QString("entry063.txt"));
        auto Last = Delete("entry119.txt", {});
        QTRY_VERIFY(Last.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        const auto LastJob = Last.get(); QVERIFY(LastJob.HasValue());
        QTRY_VERIFY_WITH_TIMEOUT(IsTerminal(Bridge->FileJobs()->Scheduler().Snapshot(LastJob.Value().jobId_).Value().state_), 10000);
        QTRY_VERIFY(!Model->IsLoading() && Model->rowCount() == 116);
        QTRY_COMPARE(FocusName(), QString("entry118.txt"));
        auto Other = Delete("entry070.txt", {"entry005.txt"});
        QTRY_VERIFY(Other.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        const auto OtherJob = Other.get(); QVERIFY(OtherJob.HasValue());
        QTRY_VERIFY_WITH_TIMEOUT(IsTerminal(Bridge->FileJobs()->Scheduler().Snapshot(OtherJob.Value().jobId_).Value().state_), 10000);
        QTRY_VERIFY(!Model->IsLoading() && Model->rowCount() == 115);
        QTRY_COMPARE(FocusName(), QString("entry070.txt"));
    }

    void itemSelectionUi()
    {
        auto& Settings = hc::settings::AppSettings();
        const auto OriginalOptions = Settings.Read(hc::settings::Section::Options);
        const auto RestoreOptions = qScopeGuard([&] { Settings.Put(hc::settings::Section::Options, OriginalOptions); });
        const QJsonObject DocumentRule{{"id", "selection-documents"}, {"name", "업무 문서"}, {"enabled", true}, {"join", "all"},
            {"conditions", QJsonArray{QJsonObject{{"type", "extension"}, {"op", "in"}, {"value", "pdf,hwpx,txt"}}}}};
        const QJsonObject ImageRule{{"id", "selection-images"}, {"name", "검토 이미지"}, {"enabled", true}, {"join", "all"},
            {"conditions", QJsonArray{QJsonObject{{"type", "extension"}, {"op", "in"}, {"value", "png"}}}}};
        auto Options = OriginalOptions.toObject();
        Options["fileColorRules"] = QJsonObject{{"version", 2}, {"mode", "first"}, {"groups", QJsonArray{DocumentRule, ImageRule}}};
        QVERIFY(Settings.Put(hc::settings::Section::Options, Options));
        auto General = Settings.Read(hc::settings::Section::General).toObject();
        General["selectionDialog"] = QJsonObject{{"last", QJsonObject{{"groupId", "builtin.document"}, {"pattern", "*.pdf"}}}};
        QVERIFY(Settings.Put(hc::settings::Section::General, General));
        const auto Workspace = main_->CaptureWorkspace();
        const auto Restore = qScopeGuard([&] { main_->RestoreWorkspace(Workspace); });
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        for (const auto& Name : {"report.pdf", "notes.hwpx", "todo.txt", "image.png", "noextension"}) {
            QFile File(Directory.filePath(Name)); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("selection fixture");
        }
        QVERIFY(QDir(Directory.path()).mkdir("folder.pdf"));
        auto* Panel = panels_.front(); Panel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {});
        QTRY_VERIFY(!model(Panel)->IsLoading()); QTRY_COMPARE(model(Panel)->rowCount(), 7);
        Panel->sig_NotifyPanelActivated();
        auto* View = qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex())); QVERIFY(View);
        auto* Grid = View->BaseView();
        const auto Row = [&](const QString& Name) {
            for (int I = 0; I < Grid->getRowCount(); ++I) {
                const auto GridRow = Grid->getRow(I); auto Index = GridRow.modelIndex(0);
                while (const auto* Proxy = qobject_cast<const QAbstractProxyModel*>(Index.model())) Index = Proxy->mapToSource(Index);
                if (Index.isValid() && model(Panel)->GetFileInfo(Index)->Name_ == Name) return GridRow.rowIndex();
            }
            return -1;
        };
        QVERIFY(Row("image.png") >= 0); Grid->deselectAll(); Grid->selectRow(Row("image.png"), Qtitan::Select);
        QCOMPARE(View->CaptureSelection().size(), 1);
        const auto DialogKey = [&](Qt::Key Key, Qt::KeyboardModifiers Mods, const QString& Capture, bool ApplyThenCancel = false) {
            QTimer Timer; Timer.setInterval(15); int Stage = 0, Ticks = 0; bool Completed = false;
            connect(&Timer, &QTimer::timeout, main_, [&] {
                auto* Dialog = main_->findChild<QDialog*>("hc.selection.dialog");
                if (++Ticks > 800) { Completed = false; if (Dialog) Dialog->reject(); Timer.stop(); return; }
                if (!Dialog) return;
                if (Stage == 4) return;
                if (Stage == 3) {
                    Completed = Dialog->isVisible(); Stage = 4;
                    QTest::keyClick(Dialog, Qt::Key_C, Qt::AltModifier); return;
                }
                auto* Group = Dialog->findChild<QComboBox*>("hc.selection.group");
                auto* Accept = Dialog->findChild<QPushButton*>("hc.dialog.accept");
                if (!Stage) {
                    if (Group->count() != 3 || Group->itemText(1) != QStringLiteral("업무 문서") || Group->itemText(2) != QStringLiteral("검토 이미지")) {
                        Dialog->reject(); Timer.stop(); return;
                    }
                    Group->setCurrentIndex(Group->findData("user:selection-documents")); Stage = 1; return; }
                if (!Accept->isEnabled()) return;
                const auto Preview = Dialog->findChild<QLabel*>("hc.selection.preview")->text();
                if (!Preview.startsWith(QStringLiteral("일치 3개"))) return;
                const auto Out = qEnvironmentVariable("HC_UI_ARTIFACT_DIR");
                if (Stage == 1 && !Out.isEmpty() && !Capture.isEmpty()) {
                    Timer.stop();
                    QDir().mkpath(Out); auto& Theme = fm::style::ThemeManager::instance(); const auto Before = Theme.scheme();
                    for (const bool Dark : {true, false}) {
                        Theme.setScheme(Dark ? fm::style::ThemeManager::Scheme::Dark : fm::style::ThemeManager::Scheme::Light);
                        QTest::qWait(50);
                        Dialog->grab().save(QDir(Out).filePath(Capture + (Dark ? "-dark-" : "-light-") + QString::number(Dialog->devicePixelRatioF()) + ".png"));
                    }
                    Theme.setScheme(Before);
                    Stage = 2; Timer.start();
                    Group->setCurrentIndex(0); Group->setCurrentIndex(Group->findData("user:selection-documents"));
                    return;
                }
                if (ApplyThenCancel) {
                    Stage = 3; QTest::keyClick(Dialog, Qt::Key_A, Qt::AltModifier);
                } else {
                    Completed = true; Stage = 4;
                    QTest::keyClick(Dialog, Key == Qt::Key_Minus ? Qt::Key_D : Qt::Key_S, Qt::AltModifier);
                }
            });
            Timer.start(); View->EnsureKeyboardFocusOnView();
            auto* Target = View->IsThumbnailMode() ? View->findChild<QWidget*>("hc.thumbnailTiles") : View->findChild<QWidget*>("grid");
            if (!Target) Target = View->findChild<Qtitan::Grid*>();
            QTest::keyClick(Target, Key, Mods);
            Timer.stop(); return Completed;
        };
        QVERIFY(DialogKey(Qt::Key_Equal, Qt::ControlModifier, "selection-document-add", true));
        QTRY_COMPARE(View->CaptureSelection().size(), 4);
        QVERIFY(DialogKey(Qt::Key_Minus, Qt::ControlModifier, "selection-document-remove"));
        QTRY_COMPARE(View->CaptureSelection().size(), 1);
        QVERIFY(DialogKey(Qt::Key_Plus, Qt::KeypadModifier, {})); QTRY_COMPARE(View->CaptureSelection().size(), 4);
        QVERIFY(DialogKey(Qt::Key_Minus, Qt::KeypadModifier, {})); QTRY_COMPARE(View->CaptureSelection().size(), 1);
        QVERIFY(hc::settings::AppSettings().Error().isEmpty());
        QCOMPARE(hc::settings::AppSettings().Read(hc::settings::Section::General).toObject()["selectionDialog"].toObject()["last"].toObject()["groupId"].toString(), QString("user:selection-documents"));
        auto* Bridge = TyStCommandMgr::GetInstance()->Bridge();
        QVERIFY(Bridge->ExecuteCommand("hc.selection.restore")); QTRY_COMPARE(View->CaptureSelection().size(), 4);
        QVERIFY(Bridge->ExecuteCommand("hc.selection.clear")); QTRY_COMPARE(View->CaptureSelection().size(), 0);
        QVERIFY(Bridge->ExecuteCommand("hc.selection.all")); QTRY_COMPARE(View->CaptureSelection().size(), 6);
        QVERIFY(model(Panel)->property("hc.selectAllEntries").toBool() == false);
        Grid->setFocusedRowIndex(Row("report.pdf"));
        QVERIFY(Bridge->ExecuteCommand("hc.selection.clearExtension")); QTRY_COMPARE(View->CaptureSelection().size(), 5);
        QVERIFY(!Grid->modelController()->isRowSelected(Grid->getRow(Row(".."))));
        QVERIFY(Bridge->ExecuteCommand("hc.selection.remember"));
        QTRY_VERIFY(main_->statusBar()->currentMessage().contains(QStringLiteral("현재 선택을 기억")));
        QVERIFY(Bridge->ExecuteCommand("hc.selection.clear")); QTRY_COMPARE(View->CaptureSelection().size(), 0);
        QVERIFY(Bridge->ExecuteCommand("hc.selection.recall")); QTRY_COMPARE(View->CaptureSelection().size(), 5);
        QVERIFY(Bridge->ExecuteCommand("hc.selection.invertAll")); QTRY_COMPARE(View->CaptureSelection().size(), 1);
        QTemporaryDir CopiedFiles; QVERIFY(CopiedFiles.isValid());
        auto Copy = Bridge->Dispatch("hc.file.copy", {{"destination", CopiedFiles.path()}}, "selection-test");
        QTRY_VERIFY(Copy.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        const auto Copied = Copy.get(); QVERIFY(Copied.HasValue());
        auto& Scheduler = Bridge->FileJobs()->Scheduler();
        QTRY_VERIFY_WITH_TIMEOUT(hc::operations::IsTerminal(Scheduler.Snapshot(Copied.Value().jobId_).Value().state_), 10000);
        QCOMPARE(Scheduler.Snapshot(Copied.Value().jobId_).Value().state_, hc::operations::JobState::Completed);
        QCOMPARE(QDir(CopiedFiles.path()).entryList(QDir::Files | QDir::NoDotAndDotDot), QStringList{"report.pdf"});
        QTRY_VERIFY(!model(Panel)->IsLoading()); QTRY_COMPARE(View->CaptureSelection().size(), 1);
        QVERIFY(View->SetListMode("thumbnails"));
        QVERIFY(DialogKey(Qt::Key_Equal, Qt::ControlModifier, "selection-thumbnail-add"));
        QTRY_COMPARE(View->CaptureSelection().size(), 3);
        QVERIFY(View->SetListMode("one-line"));
        const auto Keys = View->CaptureSelection();
        QTimer Cancel; Cancel.setInterval(15); int CancelTicks = 0;
        connect(&Cancel, &QTimer::timeout, main_, [&] {
            if (++CancelTicks > 800) { Cancel.stop(); return; }
            if (auto* Dialog = main_->findChild<QDialog*>("hc.selection.dialog")) { Cancel.stop(); Dialog->reject(); }
        });
        Cancel.start(); QVERIFY(Bridge->ExecuteCommand("hc.selection.deselect")); Cancel.stop();
        QCOMPARE(View->CaptureSelection(), Keys);
        auto ChangedRule = DocumentRule; ChangedRule["name"] = "PDF 문서";
        ChangedRule["conditions"] = QJsonArray{QJsonObject{{"type", "extension"}, {"op", "in"}, {"value", "pdf"}}};
        Options["fileColorRules"] = QJsonObject{{"version", 2}, {"mode", "first"}, {"groups", QJsonArray{ImageRule, ChangedRule}}};
        QVERIFY(Settings.Put(hc::settings::Section::Options, Options));
        QTimer ReloadGroups; ReloadGroups.setInterval(15); int ReloadTicks = 0; bool Reloaded = false;
        connect(&ReloadGroups, &QTimer::timeout, main_, [&] {
            auto* Dialog = main_->findChild<QDialog*>("hc.selection.dialog");
            if (++ReloadTicks > 600) { if (Dialog) Dialog->reject(); ReloadGroups.stop(); return; }
            if (!Dialog) return;
            const auto Preview = Dialog->findChild<QLabel*>("hc.selection.preview")->text();
            if (!Preview.startsWith(QStringLiteral("일치 1개"))) return;
            auto* Group = Dialog->findChild<QComboBox*>("hc.selection.group");
            Reloaded = Group->count() == 3 && Group->itemText(1) == QStringLiteral("검토 이미지")
                && Group->currentText() == QStringLiteral("PDF 문서");
            ReloadGroups.stop(); Dialog->reject();
        });
        ReloadGroups.start(); QVERIFY(Bridge->ExecuteCommand("hc.selection.deselect")); ReloadGroups.stop(); QVERIFY(Reloaded);
        QCOMPARE(View->CaptureSelection(), Keys);
        QTemporaryDir Other; QVERIFY(Other.isValid());
        { QFile File(Other.filePath("other.pdf")); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("other location"); }
        QTimer Navigate; Navigate.setInterval(20); int NavigationStage = 0, NavigationTicks = 0; bool RejectedStale = false;
        connect(&Navigate, &QTimer::timeout, main_, [&] {
            auto* Dialog = main_->findChild<QDialog*>("hc.selection.dialog");
            if (++NavigationTicks > 600) { if (Dialog) Dialog->reject(); Navigate.stop(); return; }
            if (!Dialog) return;
            if (!NavigationStage) { Panel->ChangeCurrentPath(QDir::toNativeSeparators(Other.path()), {}); NavigationStage = 1; return; }
            if (model(Panel)->IsLoading() || NavigationTicks < 25) return;
            if (NavigationStage == 1) { Dialog->findChild<QLineEdit*>("hc.selection.pattern")->setText("*.pdf"); NavigationStage = 2; NavigationTicks = 0; return; }
            RejectedStale = !Dialog->findChild<QPushButton*>("hc.dialog.accept")->isEnabled(); Navigate.stop(); Dialog->reject();
        });
        Navigate.start(); QVERIFY(Bridge->ExecuteCommand("hc.selection.select")); Navigate.stop(); QVERIFY(RejectedStale);
        QCOMPARE(View->CaptureSelection().size(), 0);
    }

    void todoBugFileAttributesCommand()
    {
        const auto Workspace = main_->CaptureWorkspace();
        const auto Restore = qScopeGuard([&] { main_->RestoreWorkspace(Workspace); });
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        QFile File(Directory.filePath("attributes.txt")); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("unchanged"); File.close();
        auto* Panel = panels_.front(); Panel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {});
        QTRY_VERIFY(!model(Panel)->IsLoading()); QTRY_COMPARE(model(Panel)->rowCount(), 2);
        auto* View = static_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex()));
        auto* Bridge = TyStCommandMgr::GetInstance()->Bridge(); QVERIFY(Bridge);
        auto* Action = Bridge->Action("hc.file.attributes"); QVERIFY(Action);
        for (bool Shortcut : {false, true}) {
            main_->raise(); main_->activateWindow(); QVERIFY(QTest::qWaitForWindowActive(main_));
            View->BaseView()->setFocusedRowIndex(1); emit Panel->sig_NotifyPanelActivated(); View->EnsureKeyboardFocusOnView();
            QTRY_VERIFY(Action->isEnabled()); QVERIFY(QApplication::focusWidget());
            bool Opened = false; QElapsedTimer Elapsed; Elapsed.start();
            QTimer Probe; Probe.setInterval(30);
            connect(&Probe, &QTimer::timeout, main_, [&] {
                for (auto* Window : QApplication::topLevelWidgets()) {
                    if (Window->objectName() != "hc.fileAttributes") continue;
                    auto* Value = Window->findChild<QComboBox*>("hc.attributes.value0");
                    if ((Value && Value->isEnabled()) || Elapsed.elapsed() > 5000) {
                        Opened = Value && Value->isEnabled(); qobject_cast<QDialog*>(Window)->reject();
                    }
                }
            }); Probe.start();
            if (Shortcut) QTest::keyClick(QApplication::focusWidget(), Qt::Key_Z, Qt::ControlModifier);
            else Action->trigger();
            QTRY_VERIFY_WITH_TIMEOUT(Opened, 6000); Probe.stop();
        }
        QVERIFY(File.open(QIODevice::ReadOnly)); QCOMPARE(File.readAll(), QByteArray("unchanged"));
    }


    void todoBugFavoriteSafety()
    {
        auto& Settings = hc::settings::AppSettings();
        const auto Before = Settings.Object();
        auto* Favorites = TyStFavoriteDirMgr::GetInstance();
        const auto Restore = qScopeGuard([&] { Settings.Replace(Before); Favorites->Refresh(); });
        QVERIFY(Settings.Put(hc::settings::Section::Favorites, QJsonObject{{"items", QJsonArray{}}, {"drivesOn", "none"}}));
        Favorites->Refresh();
        QFavoriteDirConfigure Dialog;
        auto* List = Dialog.findChild<QListWidget*>("lstDir"); QVERIFY(List);
        QVERIFY(!Dialog.findChild<QLineEdit*>("edtPath")->isEnabled());
        QVERIFY(QMetaObject::invokeMethod(&Dialog, "on_edtCommand_editingFinished"));
        QVERIFY(QMetaObject::invokeMethod(&Dialog, "on_edtPath_editingFinished"));
        auto* Item = new QListWidgetItem("temporary", List);
        List->setCurrentItem(Item);
        List->setCurrentItem(nullptr);
        QVERIFY(!Dialog.findChild<QPushButton*>("btnRemove")->isEnabled());
        QVERIFY(QMetaObject::invokeMethod(&Dialog, "on_btnOK_clicked", Q_ARG(bool, false)));
        QCOMPARE(Dialog.result(), int(QDialog::Accepted));
        QCOMPARE(Favorites->GetCount(), qsizetype(0));
    }

    void todoBugFavoriteDraft()
    {
        auto& Settings = hc::settings::AppSettings(); const auto Before = Settings.Object();
        auto* Favorites = TyStFavoriteDirMgr::GetInstance();
        const auto Restore = qScopeGuard([&] { Settings.Replace(Before); Favorites->Refresh(); });
        QVERIFY(Favorites->ApplyConfiguration({{"first", "command", "C:/original"}}, CFavoriteDirMgr::DRIVES_ON_TOP));
        const auto Original = Settings.Read(hc::settings::Section::Favorites);
        for( int Exit = 0; Exit < 3; ++Exit ) {
            QFavoriteDirConfigure Dialog;
            QCOMPARE(Settings.Read(hc::settings::Section::Favorites), Original);
            auto* List = Dialog.findChild<QListWidget*>("lstDir"); List->setCurrentRow(0);
            Dialog.findChild<QLineEdit*>("edtPath")->setText("C:/changed");
            QVERIFY(QMetaObject::invokeMethod(&Dialog, "on_edtPath_editingFinished"));
            Dialog.findChild<QRadioButton*>("rdoDrivesOnBottom")->setChecked(true);
            QVERIFY(QMetaObject::invokeMethod(&Dialog, "on_btnRemove_clicked", Q_ARG(bool, false)));
            QTimer::singleShot(0, [] { if (auto* Input = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) { Input->setTextValue("new"); Input->accept(); } });
            QVERIFY(QMetaObject::invokeMethod(&Dialog, "on_btnAdd_clicked", Q_ARG(bool, false)));
            QCOMPARE(List->count(), 1); QCOMPARE(List->currentRow(), 0);
            QCOMPARE(Settings.Read(hc::settings::Section::Favorites), Original);
            QCOMPARE(Favorites->GetItem(0).Name, QString("first"));
            if( Exit == 0 ) QVERIFY(QMetaObject::invokeMethod(&Dialog, "on_btnCancel_clicked", Q_ARG(bool, false)));
            else if( Exit == 1 ) Dialog.reject();
            else { Dialog.show(); Dialog.close(); }
            Favorites->Refresh(); QCOMPARE(Settings.Read(hc::settings::Section::Favorites), Original);
            QCOMPARE(Favorites->GetItem(0).Path, QString("C:/original"));
        }
        QFavoriteDirConfigure Dialog;
        Dialog.findChild<QListWidget*>("lstDir")->setCurrentRow(0);
        Dialog.findChild<QLineEdit*>("edtPath")->setText("C:/accepted");
        Dialog.findChild<QRadioButton*>("rdoDrivesOnBottom")->setChecked(true);
        QVERIFY(QMetaObject::invokeMethod(&Dialog, "on_btnOK_clicked", Q_ARG(bool, false)));
        Favorites->Refresh(); QCOMPARE(Favorites->GetItem(0).Path, QString("C:/accepted"));
        QCOMPARE(Favorites->GetDrivesOn(), CFavoriteDirMgr::DRIVES_ON_BOTTOM);
    }

    void todoBugFavoriteRename()
    {
        auto& Settings = hc::settings::AppSettings(); const auto Before = Settings.Object();
        auto* Favorites = TyStFavoriteDirMgr::GetInstance();
        const auto Restore = qScopeGuard([&] { Settings.Replace(Before); Favorites->Refresh(); });
        QVERIFY(Favorites->ApplyConfiguration({{"first", "command", "C:/original"}, {"second", "", ""}}, CFavoriteDirMgr::DRIVES_ON_NONE));
        QFavoriteDirConfigure Dialog;
        auto* List = Dialog.findChild<QListWidget*>("lstDir"); List->setCurrentRow(0);
        QStringList Names{"   ", "SECOND", "renamed"};
        QTimer Timer; Timer.setInterval(20);
        connect(&Timer, &QTimer::timeout, &Dialog, [&] {
            if(auto* Input = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
                if(Names.isEmpty()) Input->reject(); else { Input->setTextValue(Names.takeFirst()); Input->accept(); }
            } else if(auto* Message = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) Message->accept();
        });
        Timer.start();
        QVERIFY(QMetaObject::invokeMethod(&Dialog, "on_btnRename_clicked", Q_ARG(bool, false)));
        Timer.stop(); QVERIFY(Names.isEmpty());
        QCOMPARE(List->currentItem()->text(), QString("renamed")); QCOMPARE(List->currentRow(), 0);
        QCOMPARE(Favorites->GetItem(0).Name, QString("first"));
        QVERIFY(QMetaObject::invokeMethod(&Dialog, "on_btnOK_clicked", Q_ARG(bool, false)));
        Favorites->Refresh(); QCOMPARE(Favorites->GetItem(0).Name, QString("renamed"));
        QCOMPARE(Favorites->GetItem(0).Path, QString("C:/original"));
        QCOMPARE(Favorites->GetItem(0).Command, QString("command"));
        QCOMPARE(Favorites->GetItem(1).Name, QString("second"));
    }

    void todoBugColumnDraft()
    {
        using namespace hc::columns;
        auto First = DefaultProfile(); First.id_ = QUuid::createUuid(); First.title_ = "first";
        auto Second = First; Second.id_ = QUuid::createUuid(); Second.title_ = "second";
        const auto FirstId = First.id_.toString(QUuid::WithoutBraces);
        const auto SecondId = Second.id_.toString(QUuid::WithoutBraces);
        QtColumnProfileEditor Editor;
        Editor.Load(First, {{FirstId, EncodeProfile(First)}, {SecondId, EncodeProfile(Second)}});
        auto* Profiles = Editor.findChild<QListWidget*>("hc.columns.profileList"); QVERIFY(Profiles);
        auto* Minimum = Editor.findChild<QDoubleSpinBox*>("hc.columns.minimum"); QVERIFY(Minimum);
        auto* Maximum = Editor.findChild<QDoubleSpinBox*>("hc.columns.maximum"); QVERIFY(Maximum);
        int Next = -1;
        for(int Index = 0; Index < Profiles->count(); ++Index) if(Profiles->item(Index)->text() == "second") Next = Index;
        QVERIFY(Next >= 0);
        Maximum->setValue(100); Minimum->setValue(200);
        const auto InvalidDraft = EncodeProfile(Editor.Profile());
        Profiles->setCurrentRow(Next);
        QCOMPARE(Editor.Profile().id_, First.id_);
        QCOMPARE(EncodeProfile(Editor.Profile()), InvalidDraft);
        QCOMPARE(Profiles->currentItem()->text(), QString("first"));
        QVERIFY(!Editor.findChild<QLabel*>("hc.columns.error")->text().isEmpty());
        QCOMPARE(Minimum->value(), 200.0);
        Minimum->setValue(50); Profiles->setCurrentRow(Next);
        QCOMPARE(Editor.Profile().id_, Second.id_);
        for(int Index = 0; Index < Profiles->count(); ++Index) if(Profiles->item(Index)->text() == "first") { Profiles->setCurrentRow(Index); break; }
        QCOMPARE(Minimum->value(), 50.0); QCOMPARE(Maximum->value(), 100.0);
    }

    void todoBugNavigationPreparation()
    {
        const auto Workspace = main_->CaptureWorkspace();
        const auto Restore = qScopeGuard([&] { main_->RestoreWorkspace(Workspace); });
        auto* Model = static_cast<CFSModel*>(model(panels_.front()));
        QTRY_VERIFY(!Model->IsLoading());
        TyStCommandMgr::GetInstance()->Bridge()->Capture(); // Bind the current workspace models before observing command events.
        QSignalSpy Failed(Model, &CFsModelT::sigNavigationFailed);
        const auto Location = Model->WorkspaceLocation();
        Model->setProperty("hc.correlation", "pending-navigation-test");
        Model->SetPlaceholder(Location, "connecting", true);
        QCOMPARE(Failed.size(), 0);
        QVERIFY(!Model->LastNavigationError());
        QCOMPARE(Model->property("hc.correlation").toString(), QString("pending-navigation-test"));
        Model->SetPlaceholder(Location, "real-failure");
        QCOMPARE(Failed.size(), 1);
        QVERIFY(Model->LastNavigationError());
        QCOMPARE(Model->property("hc.lastNavigationError").toString(), QString("real-failure"));
        QVERIFY(Model->property("hc.correlation").toString().isEmpty());
    }

    void todoBugCopyVerificationText()
    {
        auto& Settings = hc::settings::AppSettings(); const auto Before = Settings.Object();
        const auto Restore = qScopeGuard([&] { Settings.Replace(Before); });
        auto Options = Settings.Read(hc::settings::Section::Options).toObject();
        Options["copyVerify"] = true; QVERIFY(Settings.Put(hc::settings::Section::Options, Options));
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        for(bool Engine : {true, false}) for(bool Native : {true, false}) {
            hc::ui::FileOperationDialogRequest Request;
            Request.command_ = "hc.file.copy"; Request.source_ = Directory.path(); Request.initialValue_ = Directory.path();
            Request.names_ = {"sample.txt"}; Request.nativeSource_ = true;
            Request.nativeDestination_ = Native; Request.copyEngine_ = Engine;
            hc::ui::FileOperationDialog Dialog(Request);
            auto* Check = Dialog.findChild<QCheckBox*>("hc.operation.verify"); QVERIFY(Check);
            auto* Label = Dialog.findChild<QLabel*>("hc.operation.completion"); QVERIFY(Label);
            QCOMPARE(Dialog.VerifyCopy(), Engine && Native);
            QCOMPARE(Label->text().contains("SHA-256"), Dialog.VerifyCopy());
            Check->setChecked(false);
            QVERIFY(!Dialog.VerifyCopy()); QVERIFY(!Label->text().contains("SHA-256"));
            Check->setChecked(true);
            QCOMPARE(Label->text().contains("SHA-256"), Dialog.VerifyCopy());
        }
    }

    void todoBugFavoriteSaveFailure()
    {
        auto& Settings = hc::settings::AppSettings(); const auto Before = Settings.Object();
        auto* Favorites = TyStFavoriteDirMgr::GetInstance();
        const auto Restore = qScopeGuard([&] { Settings.Replace(Before); Favorites->Refresh(); });
        QTimer ErrorCloser; ErrorCloser.setInterval(10);
        connect(&ErrorCloser, &QTimer::timeout, main_, [] {
            for(auto* Widget : QApplication::topLevelWidgets())
                if(auto* Message = qobject_cast<QMessageBox*>(Widget)) Message->accept();
        });
        ErrorCloser.start();
        const auto DrainErrors = qScopeGuard([] { QTest::qWait(100); });
        QVERIFY(Favorites->ApplyConfiguration({{"original", "", "C:/original"}}, CFavoriteDirMgr::DRIVES_ON_TOP));
        const auto Original = Settings.Read(hc::settings::Section::Favorites);
        {
            QLockFile Lock(Settings.FileName() + ".lock"); Lock.setStaleLockTime(0); QVERIFY(Lock.tryLock(0));
            QVERIFY(!Favorites->ApplyConfiguration({{"failed", "", "C:/failed"}}, CFavoriteDirMgr::DRIVES_ON_BOTTOM));
            QCOMPARE(Favorites->GetItem(0).Name, QString("original"));
            QCOMPARE(Favorites->GetDrivesOn(), CFavoriteDirMgr::DRIVES_ON_TOP);
            QCOMPARE(Settings.Read(hc::settings::Section::Favorites), Original);
        }
        QVERIFY(Settings.Commit()); Favorites->Refresh();
        QCOMPARE(Favorites->GetItem(0).Name, QString("original"));
    }

    void r6FileDialogsUi()
    {
        using namespace hc::ui;
        qInfo() << "R6 application style" << QApplication::style()->metaObject()->className();
        if (auto* Proxy = qobject_cast<QProxyStyle*>(QApplication::style())) qInfo() << "R6 base style" << Proxy->baseStyle()->metaObject()->className();
        QTemporaryDir Directory; QVERIFY(Directory.isValid()); QVERIFY(QDir(Directory.path()).mkpath("existing/child"));
        QFile File(Directory.filePath("sample.txt")); QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write("hello"), 5); File.close();
        const auto Before = hc::settings::AppSettings().Object();
        const auto Screenshot = [&](FileOperationDialog& Dialog, const QString& Name) {
            const auto Artifacts = qEnvironmentVariable("HC_UI_ARTIFACT_DIR");
            if (Artifacts.isEmpty()) return true;
            auto& Theme = fm::style::ThemeManager::instance(); const auto Original = Theme.scheme();
            const auto Restore = qScopeGuard([&] { Theme.setScheme(Original); });
            const bool LightSaved = Dialog.grab().save(QDir(Artifacts).filePath(Name));
            Theme.setScheme(fm::style::ThemeManager::Scheme::Dark); QTest::qWait(60);
            auto DarkName = Name; DarkName.replace("-light", "-dark");
            return LightSaved && Dialog.grab().save(QDir(Artifacts).filePath(DarkName));
        };
        FileOperationDialogRequest Request; Request.source_ = Directory.path(); Request.nativeSource_ = true; Request.nativeDestination_ = true; Request.providerKey_ = "local"; Request.names_ = {"sample.txt"};
        Request.command_ = "hc.file.copy"; Request.initialValue_ = Directory.filePath("existing");
        { FileOperationDialog Dialog(Request, main_); Dialog.show();
          auto* Summary = Dialog.findChild<QLabel*>("hc.operation.summary"); QTRY_VERIFY(Summary->text().contains("파일 1개")); QVERIFY(Summary->text().contains("5"));
          auto* Editor = Dialog.findChild<QLineEdit*>("hc.operation.editor");
          auto* Target = Dialog.findChild<QLabel*>("hc.operation.targetSummary");
          Editor->setText(Directory.filePath("obsolete/missing")); Editor->setText(Directory.filePath("existing"));
          QTRY_COMPARE(Target->property("target").toString(), QDir::toNativeSeparators(Directory.filePath("existing"))); QVERIFY(Target->text().contains("사용 가능"));
          QVERIFY(Screenshot(Dialog, "r6-copy-light.png"));
          auto* Scroll = Dialog.findChild<QScrollArea*>(); QVERIFY(Scroll); Scroll->verticalScrollBar()->setValue(Scroll->verticalScrollBar()->maximum()); QTest::qWait(30);
          QVERIFY(Screenshot(Dialog, "r6-copy-details-light.png")); Scroll->verticalScrollBar()->setValue(0);
          Dialog.findChild<QComboBox*>("hc.operation.targetMode")->setCurrentIndex(2); Editor->setText("existing/new-name.txt");
          QVERIFY(Dialog.TargetIsName()); QVERIFY(Dialog.AcceptButton()->isEnabled()); QCOMPARE(Dialog.ResolvedTarget(), QDir::toNativeSeparators(Directory.filePath("existing/new-name.txt")));
          Dialog.reject(); }
        Request.command_ = "hc.file.mkdir"; Request.initialValue_ = "existing/new/last";
        { FileOperationDialog Dialog(Request, main_); Dialog.show();
          auto* Target = Dialog.findChild<QLabel*>("hc.operation.targetSummary"); QTRY_VERIFY(Target->property("target").isValid());
          auto* Plan = Dialog.findChild<fm::ui::FolderPlanView*>(); QVERIFY(Plan); const auto Nodes = Plan->plan(); QCOMPARE(Nodes.size(), 4); QCOMPARE(Nodes[1].state, fm::ui::FolderPlanView::Node::Existing); QCOMPARE(Nodes[2].state, fm::ui::FolderPlanView::Node::New);
          QVERIFY(Dialog.CreateParents()); Dialog.findChild<QCheckBox*>("hc.operation.openCreated")->setChecked(true); QVERIFY(Dialog.OpenCreatedFolder());
          QVERIFY(Dialog.AcceptButton()->isEnabled()); QVERIFY(Screenshot(Dialog, "r6-mkdir-light.png"));
          auto* Editor = Dialog.findChild<QLineEdit*>("hc.operation.editor"); Editor->setText("../escape"); QVERIFY(Dialog.AcceptButton()->isEnabled()); QCOMPARE(QDir::fromNativeSeparators(Dialog.ResolvedTarget()), QDir::cleanPath(Directory.path()+"/../escape")); Editor->setText("CON/child"); QVERIFY(!Dialog.AcceptButton()->isEnabled()); Dialog.reject(); }
        Request.command_ = "hc.file.rename"; Request.initialValue_ = "sample.txt";
        { FileOperationDialog Dialog(Request, main_); Dialog.show(); auto* Editor = Dialog.findChild<QLineEdit*>("hc.operation.editor"); Editor->setText("existing/renamed.txt"); QVERIFY(Dialog.AcceptButton()->isEnabled());
          Dialog.findChild<QCheckBox*>("hc.operation.parents")->setChecked(true); QVERIFY(Dialog.CreateParents()); Editor->setText("existing/renamed.bin"); QVERIFY(!Dialog.AcceptButton()->isEnabled());
          for (auto* Check : Dialog.findChildren<QCheckBox*>()) if (Check->text().contains("확장자")) Check->setChecked(true);
          QVERIFY(Dialog.AcceptButton()->isEnabled()); Editor->setText("existing/renamed.txt"); QTest::qWait(200); QVERIFY(Screenshot(Dialog, "r6-rename-light.png")); Dialog.reject(); }
        Request.command_ = "hc.file.move"; Request.initialValue_ = Directory.filePath("existing");
        { FileOperationDialog Dialog(Request, main_); Dialog.show(); Dialog.findChild<QComboBox*>("hc.operation.targetMode")->setCurrentIndex(2);
          Dialog.findChild<QLineEdit*>("hc.operation.editor")->setText("existing/new/renamed.txt"); Dialog.findChild<QCheckBox*>("hc.operation.parents")->setChecked(true);
          QTRY_VERIFY(Dialog.findChild<QLabel*>("hc.operation.targetSummary")->property("target").isValid()); QVERIFY(Screenshot(Dialog, "r6-move-light.png")); Dialog.reject(); }
        Request.command_ = "hc.file.delete";
        { FileOperationDialog Dialog(Request, main_); Dialog.show(); auto* Recycle = Dialog.findChild<QRadioButton*>("hc.operation.recycle"); QTRY_VERIFY(Recycle->isEnabled()); QVERIFY(Dialog.Recycle()); QVERIFY(Dialog.AcceptButton()->isEnabled());
          QVERIFY(Screenshot(Dialog, "r6-delete-recycle-light.png")); Dialog.findChild<QRadioButton*>("hc.operation.permanent")->setChecked(true); QVERIFY(!Dialog.Recycle());
          Dialog.findChild<QCheckBox*>("hc.operation.readonly")->setChecked(true); QVERIFY(Dialog.AllowReadOnly()); QVERIFY(Screenshot(Dialog, "r6-delete-permanent-light.png")); Dialog.reject(); }
        Request.nativeSource_ = false;
        { FileOperationDialog Dialog(Request, main_); Dialog.show(); QTest::qWait(100); QVERIFY(!Dialog.AcceptButton()->isEnabled()); QVERIFY(!Dialog.Recycle());
          Dialog.findChild<QRadioButton*>("hc.operation.permanent")->setChecked(true); QVERIFY(Dialog.AcceptButton()->isEnabled()); Dialog.reject(); }
        Request.command_ = "hc.file.copy"; Request.nativeSource_ = true; Request.initialValue_ = Directory.filePath("existing"); Request.names_.append("existing");
        { FileOperationDialog Dialog(Request, main_); Dialog.show(); QTRY_VERIFY(Dialog.findChild<QLabel*>("hc.operation.summary")->text().contains("폴더 2개"));
          auto* Mode=Dialog.findChild<QComboBox*>("hc.operation.targetMode"); QVERIFY(Mode->isEnabled()); QVERIFY(!(Mode->model()->flags(Mode->model()->index(2,0))&Qt::ItemIsEnabled)); Dialog.reject(); }
        QCOMPARE(hc::settings::AppSettings().Object(), Before);
    }
    void todoBugAutomaticTransferTargetsUi()
    {
        using namespace hc::ui; using namespace hc::operations;
        auto& Settings=hc::settings::AppSettings(); const auto Before=Settings.Object(); const auto Workspace=main_->CaptureWorkspace();
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        const auto Restore=qScopeGuard([&]{Settings.Replace(Before); Settings.Commit(); main_->RestoreWorkspace(Workspace);});
        auto Options=Settings.Read(hc::settings::Section::Options).toObject(); Options["copyPattern"]="*"; Options["copySpeedMiB"]=0; Options["copyAcl"]=false; Options["copyStreams"]=false; Options["copyVerify"]=true; QVERIFY(Settings.Put(hc::settings::Section::Options,Options));
        for(auto* Panel:panels_) { if(auto* Error=Panel->findChild<QMessageBox*>("hc.navigationError"))Error->reject(); Panel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()),{}); QTRY_VERIFY(!model(Panel)->IsLoading()); }
        auto* Panel=panels_.front(); auto* Model=static_cast<CFSModel*>(model(Panel)); auto* Bridge=TyStCommandMgr::GetInstance()->Bridge(); auto* Jobs=Bridge->FileJobs(); auto& Scheduler=Jobs->Scheduler();
        const QByteArray Bytes("automatic target path\r\n");
        for(const bool Move:{false,true}) for(int Case=0;Case<7;++Case) {
            const auto Suffix=QString(Move?"m":"c")+QString::number(Case),SourceName="source-"+Suffix+(Case==6?QString{}:QString(".txt")),TargetName="f-"+Suffix;
            const auto Source=Directory.filePath(SourceName),Target=Directory.filePath(TargetName);
            if(Case==6)QVERIFY(QDir().mkpath(Source));
            { QFile File(Case==6?QDir(Source).filePath("child.txt"):Source); QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write(Bytes),qint64(Bytes.size())); }
            if(Case==2)QVERIFY(QDir().mkpath(Target));
            if(Case==3) { QFile Existing(Target); QVERIFY(Existing.open(QIODevice::WriteOnly)); Existing.write("old destination"); }
            Model->Refresh(); QTRY_VERIFY(!Model->IsLoading());
            const auto Find=[&] { for(int Row=0;Row<Model->rowCount();++Row)if(Model->GetName(Model->index(Row,0))==SourceName)return Model->index(Row,0);return QModelIndex{}; };
            QTRY_VERIFY(Find().isValid()); emit Panel->sig_NotifyPanelActivated();
            std::set<std::string> Prior; for(const auto& Job:Scheduler.Snapshots())Prior.insert(Job.id_);
            bool Accepted=false;
            QTimer::singleShot(0,main_,[&]{
                auto* Dialog=static_cast<FileOperationDialog*>(main_->findChild<QDialog*>("hc.dialog.fileOperation")); QVERIFY(Dialog);
                const auto Close=qScopeGuard([&]{if(!Accepted)Dialog->reject();});
                auto* Mode=Dialog->findChild<QComboBox*>("hc.operation.targetMode"); QCOMPARE(Mode->currentData().toString(),QString("auto"));
                Mode->setCurrentIndex(Mode->findData(Case==4?"folder":Case==5?"name":"auto"));
                Dialog->findChild<QLineEdit*>("hc.operation.editor")->setText(".\\"+TargetName+(Case==1?"\\":""));
                Dialog->findChild<QComboBox*>("hc.operation.conflict")->setCurrentIndex(1);
                QTRY_VERIFY_WITH_TIMEOUT(Dialog->AcceptButton()->isEnabled(),5000);
                QCOMPARE(Dialog->TargetIsName(),Case==0||Case==3||Case==5||Case==6); QCOMPARE(Dialog->CreateParents(),Case==1||Case==4);
                if(!Move && Case<2)QVERIFY(SaveR9Dialog(*Dialog,Case==0?"target-new-file":"target-new-folder"));
                Accepted=true; Dialog->AcceptButton()->click();
            });
            Bridge->PromptFileOperation(Move?"hc.file.move":"hc.file.copy","test",Find()); QVERIFY(Accepted);
            std::string Id; QTRY_VERIFY(([&]{for(const auto& Job:Scheduler.Snapshots())if(!Prior.contains(Job.id_)){Id=Job.id_;return true;}return false;})());
            QTRY_COMPARE_WITH_TIMEOUT(Scheduler.Snapshot(Id).Value().state_,JobState::Completed,15000); QTRY_VERIFY(!Jobs->IsUiPending(Id));
            const auto Output=Case==1||Case==2||Case==4?QDir(Target).filePath(SourceName):Case==6?QDir(Target).filePath("child.txt"):Target;
            QFile File(Output); QVERIFY(File.open(QIODevice::ReadOnly)); QCOMPARE(File.readAll(),Bytes); QCOMPARE(QFileInfo::exists(Source),!Move);
            for(auto* Dialog:main_->findChildren<QDialog*>("hc.fileJobs"))Dialog->hide();
        }
    }
    void r6FileOperationsUi()
    {
        using namespace hc::operations; using namespace hc::commands; using namespace hc::ui;
        auto& Settings = hc::settings::AppSettings(); const auto Before = Settings.Object(); const auto Workspace = main_->CaptureWorkspace();
        const auto Restore = qScopeGuard([&] { Settings.Replace(Before); main_->RestoreWorkspace(Workspace); });
        QTemporaryDir Directory; QVERIFY(Directory.isValid()); const auto Source = Directory.path();
        QFile File(Directory.filePath("sample.txt")); QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write("hello"), 5); File.close();
        auto* Panel = panels_.front(); auto* Model = static_cast<CFSModel*>(model(Panel)); Model->SetProviderEnabled(true); Panel->ChangeCurrentPath(QDir::toNativeSeparators(Source), {}); QTRY_VERIFY(!Model->IsLoading());
        auto* Bridge = TyStCommandMgr::GetInstance()->Bridge(); auto& Scheduler = Bridge->FileJobs()->Scheduler();
        // Mkdir has no selected index: explicitly activate its intended panel.
        emit Panel->sig_NotifyPanelActivated(); QCOMPARE(Bridge->Capture().tabId_, Model->property("hc.tabId").toString().toStdString());
        const auto IndexFor = [&](const QString& Name) { for (int Row = 0; Row < Model->rowCount(); ++Row) if (Model->GetName(Model->index(Row, 0)) == Name) return Model->index(Row, 0); return QModelIndex{}; };
        auto Mkdir = Bridge->Dispatch("hc.file.mkdir", {{"destination", Directory.filePath("new/child/last")}, {"createParents", true}, {"openCreated", true}}, "test");
        QTRY_VERIFY(Mkdir.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready); auto Created = Mkdir.get(); QVERIFY(Created.HasValue());
        QTRY_COMPARE(Scheduler.Snapshot(Created.Value().jobId_).Value().state_, JobState::Completed); QTRY_COMPARE(QDir::cleanPath(Panel->CurrentPath()), QDir::cleanPath(QDir::toNativeSeparators(Directory.filePath("new/child/last")))); QTRY_VERIFY(!Model->IsLoading());
        Panel->ChangeCurrentPath(QDir::toNativeSeparators(Source), {}); QTRY_VERIFY(!Model->IsLoading());
        const auto Target = Directory.filePath("copy/new/copy.txt");
        auto WrongScope = Bridge->Dispatch("hc.file.copy", {{"destination", Target}, {"createParents", true}, {"targetIsName", true}, {"recentProvider", QString("smb")}}, "test", IndexFor("sample.txt"));
        QTRY_VERIFY(WrongScope.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready); const auto ScopeRejected = WrongScope.get(); QVERIFY(!ScopeRejected.HasValue()); QCOMPARE(ScopeRejected.Failure().code_, std::string("recent-provider-mismatch")); QVERIFY(!QFile::exists(Target));
        auto Copy = Bridge->Dispatch("hc.file.copy", {{"destination", Target}, {"createParents", true}, {"targetIsName", true}, {"recentProvider", QString("local")}}, "test", IndexFor("sample.txt"));
        QTRY_VERIFY(Copy.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready); auto Copied = Copy.get(); QVERIFY(Copied.HasValue()); QTRY_COMPARE(Scheduler.Snapshot(Copied.Value().jobId_).Value().state_, JobState::Completed);
        QFile Output(Target); QVERIFY(Output.open(QIODevice::ReadOnly)); QCOMPARE(Output.readAll(), QByteArray("hello")); Output.close(); QVERIFY(QFile::exists(Directory.filePath("sample.txt")));
        QCOMPARE(FileOperationDialog::RecentTargets("local").front(), QDir::toNativeSeparators(QFileInfo(Target).absolutePath())); QVERIFY(!FileOperationDialog::RecentTargets("smb").contains(QDir::toNativeSeparators(QFileInfo(Target).absolutePath())));
        QProcess Restart; auto Environment = QProcessEnvironment::systemEnvironment(); Environment.insert("HC_R6_RESTART", "1"); Environment.insert("HC_R6_TARGET", QDir::toNativeSeparators(QFileInfo(Target).absolutePath())); Environment.insert("HC_TEST_CONFIG_DIR", QFileInfo(Settings.FileName()).absolutePath()); Restart.setProcessEnvironment(Environment);
        Restart.start(QCoreApplication::applicationFilePath(), {"r6RecentRestartUi", "-o", "r6-restart.txt,txt"}); QVERIFY(Restart.waitForStarted(5000)); QVERIFY(Restart.waitForFinished(30000)); QCOMPARE(Restart.exitCode(), 0);
        QTest::qWait(200); QTRY_VERIFY(!Model->IsLoading()); const auto RenamedTarget = Directory.filePath("moved/new/renamed.txt");
        auto Rename = Bridge->Dispatch("hc.file.rename", {{"destination", RenamedTarget}, {"createParents", true}}, "test", IndexFor("sample.txt"));
        QTRY_VERIFY(Rename.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready); auto Renamed = Rename.get(); QVERIFY(Renamed.HasValue()); QTRY_COMPARE(Scheduler.Snapshot(Renamed.Value().jobId_).Value().state_, JobState::Completed); QVERIFY(!QFile::exists(Directory.filePath("sample.txt"))); QVERIFY(QFile::exists(RenamedTarget));
        Panel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.filePath("moved/new")), {}); QTRY_VERIFY(!Model->IsLoading());
        QVERIFY(SetFileAttributesW(QDir::toNativeSeparators(RenamedTarget).toStdWString().c_str(), FILE_ATTRIBUTE_READONLY));
        auto Delete = Bridge->Dispatch("hc.file.delete", {{"deleteMode", QString("permanent")}, {"confirmed", true}, {"allowReadOnly", true}}, "test", IndexFor("renamed.txt"));
        QTRY_VERIFY(Delete.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready); auto Deleted = Delete.get(); QVERIFY(Deleted.HasValue()); QTRY_COMPARE(Scheduler.Snapshot(Deleted.Value().jobId_).Value().state_, JobState::Completed); QVERIFY(!QFile::exists(RenamedTarget));
        QTest::qWait(200);
    }
    void r6RecentRestartUi()
    {
        if (!qEnvironmentVariableIsSet("HC_R6_RESTART")) QSKIP("Only runs as a recent-target restart child");
        QCOMPARE(hc::ui::FileOperationDialog::RecentTargets("local").front(), qEnvironmentVariable("HC_R6_TARGET"));
    }
    void thumbnailProviderUi()
    {
        using namespace fm::filelist;
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        const auto Path = Directory.filePath("image.png"); QImage Image(800, 400, QImage::Format_RGB32); Image.fill(Qt::red); QVERIFY(Image.save(Path));
        ThumbnailProvider Provider; Provider.setMethod(ThumbnailProvider::Method::Builtin); Provider.setSkipSlowVolumes(true); Provider.SetCacheLimitMb(4);
        QSignalSpy Ready(&Provider, &ThumbnailProvider::ready); const auto Time = QDateTime::currentDateTime();
        QVERIFY(Provider.thumbnail(Path, Time, 10).isNull()); QVERIFY(Provider.isPending(Path, Time, 10));
        QTRY_VERIFY(!Provider.thumbnail(Path, Time, 10).isNull());
        QCOMPARE(Provider.thumbnail(Path, Time, 10).size(), QSize(256, 128));
        QVERIFY(Provider.CacheCostKb() > 0 && Provider.CacheCostKb() <= 4096);
        Provider.clearCache(); Ready.clear();
        Provider.thumbnail(Path, Time, 10); const auto Generation = Provider.Generation(); Provider.CancelRequests();
        QCOMPARE(Provider.PendingCount(), 0); QVERIFY(Provider.Generation() > Generation);
        QTest::qWait(100); QCOMPARE(Ready.count(), 0); QCOMPARE(Provider.CacheCostKb(), 0);
        QTRY_VERIFY(!Provider.thumbnail(Path, Time, 10).isNull());
        Image.fill(Qt::blue); QVERIFY(Image.save(Path)); const auto Revised = Time.addSecs(1);
        QTRY_VERIFY(!Provider.thumbnail(Path, Revised, 11).isNull()); QCOMPARE(Provider.thumbnail(Path, Revised, 11).pixelColor(0, 0), QColor(Qt::blue));
        const auto Broken = Directory.filePath("broken.png"); QFile File(Broken); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("not an image"); File.close();
        Provider.thumbnail(Broken, Time, 12); QTRY_VERIFY(Provider.hasFailed(Broken, Time, 12));
        const auto Completed = Ready.count(); Provider.thumbnail(Broken, Time, 12); QTest::qWait(50); QCOMPARE(Ready.count(), Completed);
        QVERIFY(Image.save(Broken)); Provider.clearCache(); QTRY_VERIFY(!Provider.thumbnail(Broken, Time, 12).isNull());
        Provider.setTargets(ThumbnailProvider::Videos); QVERIFY(Provider.thumbnail(Path, Revised, 11).isNull()); QCOMPARE(Provider.PendingCount(), 0);
        QVERIFY(ThumbnailProvider::isSlowVolume("\\\\server\\share\\image.png"));
        Provider.setTargets(ThumbnailProvider::Images); Provider.thumbnail("\\\\server\\share\\image.png", Time, 1); QCOMPARE(Provider.PendingCount(), 0);
        Provider.clearCache();
        for (int Index = 0; Index < 1000; ++Index) Provider.thumbnail(Directory.filePath(QString::number(Index) + ".png"), Time, 1);
        QVERIFY(Provider.PendingCount() <= 64); Provider.CancelRequests(); QCOMPARE(Provider.PendingCount(), 0);
        // Destruction with extraction in flight joins only running jobs and drops queued jobs.
        { ThumbnailProvider Closing; Closing.setMethod(ThumbnailProvider::Method::Builtin);
          for (int Index = 0; Index < 64; ++Index) Closing.thumbnail(Path, Time.addMSecs(Index), Index); }
    }

    void thumbnailViewUi()
    {
        using namespace fm::filelist; using namespace hc::storage; using namespace hc::operations;
        auto& Settings = hc::settings::AppSettings(); const auto Original = Settings.Object(); const auto Workspace = main_->CaptureWorkspace();
        const auto Restore = qScopeGuard([&] { Settings.Replace(Original); main_->RestoreWorkspace(Workspace); });
        QTemporaryDir Directory, Destination; QVERIFY(Directory.isValid() && Destination.isValid()); QDir Dir(Directory.path()); QVERIFY(Dir.mkdir("folder"));
        for (int Index = 0; Index < 40; ++Index) {
            QImage Image(320, 180, QImage::Format_RGB32); Image.fill(QColor::fromHsv((Index * 31) % 360, 100, 220));
            QVERIFY(Image.save(Directory.filePath(QString("photo%1.png").arg(Index, 2, 10, QChar('0')))));
        }
        auto General = Settings.Read(hc::settings::Section::General).toObject(); General["thumbnails"] = QJsonObject{{"version", 1}, {"method", 1}, {"size", 96}, {"threads", 2}, {"cacheMb", 4}};
        General["directoryStyle"] = "plain"; General["iconMode"] = "standard"; QVERIFY(Settings.Put(hc::settings::Section::General, General));
        auto* Panel = panels_.front(); Panel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {}); Panel->SetSearch({});
        auto* View = static_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex())); auto* Model = static_cast<CFSModel*>(model(Panel));
        QTRY_VERIFY(!Model->IsLoading() && names(Model).contains("photo00.png")); QVERIFY(View->ApplyColumnProfile(hc::columns::DefaultProfile()));
        auto* Controls = Panel->findChild<fm::ui::SegmentedControl*>("hc.panelViewMode"); QTest::mouseClick(Controls->button(3), Qt::LeftButton);
        QTRY_VERIFY(View->IsThumbnailMode()); QCOMPARE(View->ListMode(), QString("thumbnails"));
        auto* Tiles = View->findChild<fm::filelist::ThumbnailView*>("hc.thumbnails"); auto* TileModel = View->Thumbnails(); auto* List = Tiles->listView();
        QTRY_COMPARE(TileModel->rowCount(), View->BaseView()->getRowCount());
        const auto Find = [&](const QString& Name) { for (int Row = 0; Row < TileModel->rowCount(); ++Row) if (TileModel->index(Row, 0).data(FullNameRole).toString() == Name) return Row; return -1; };
        QTRY_VERIFY(Find("photo00.png") >= 0);
        Tiles->setCursorRow(Find("photo00.png")); View->EnsureKeyboardFocusOnView(); QCoreApplication::processEvents();
        QTRY_VERIFY(!TileModel->index(Find("photo00.png"), 0).data(ThumbnailRole).value<QImage>().isNull());
        QCOMPARE(View->BaseView()->focusedRow().modelIndex(0), TileModel->SourceIndex(Tiles->cursorRow()));
        QTest::keyClick(List, Qt::Key_Right); QCOMPARE(View->BaseView()->focusedRow().modelIndex(0), TileModel->SourceIndex(Tiles->cursorRow()));
        Tiles->setCursorRow(Find("photo00.png")); QVERIFY(TileModel->setData(TileModel->index(Find("photo00.png"), 0), true, MarkedRole));
        const auto Selection = View->CaptureSelection(); const auto Focus = View->CaptureFocus(); QCOMPARE(Selection.size(), 1);
        QVERIFY(View->SetListMode("one-line")); QVERIFY(!View->IsThumbnailMode()); QCOMPARE(View->CaptureSelection(), Selection); QCOMPARE(View->CaptureFocus(), Focus);
        QVERIFY(View->SetListMode("thumbnails")); QCOMPARE(View->CaptureSelection(), Selection);
        Panel->sig_NotifyPanelActivated(); auto* Bridge = TyStCommandMgr::GetInstance()->Bridge();
        auto Copy = Bridge->Dispatch("hc.file.copy", {{"destination", Destination.path()}}, "test", TileModel->SourceIndex(Find("photo00.png")));
        QTRY_VERIFY(Copy.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready); const auto Accepted = Copy.get(); QVERIFY(Accepted.HasValue());
        auto& Scheduler = Bridge->FileJobs()->Scheduler(); QTRY_VERIFY_WITH_TIMEOUT(IsTerminal(Scheduler.Snapshot(Accepted.Value().jobId_).Value().state_), 10000);
        QCOMPARE(Scheduler.Snapshot(Accepted.Value().jobId_).Value().state_, JobState::Completed);
        { QFile A(Directory.filePath("photo00.png")), B(Destination.filePath("photo00.png")); QVERIFY(A.open(QIODevice::ReadOnly) && B.open(QIODevice::ReadOnly)); QCOMPARE(A.readAll(), B.readAll()); }
        QVERIFY(QFileInfo::exists(Destination.filePath("photo00.png"))); QVERIFY(!QFileInfo::exists(Destination.filePath("photo01.png")));
        QTRY_VERIFY(!Model->IsLoading() && Find("photo00.png") >= 0);
        View->BaseView()->deselectAll();
        Tiles->setCursorRow(Find("photo00.png")); QTest::keyClick(List, Qt::Key_Insert); QTRY_COMPARE(View->CaptureSelection().size(), 1);
        QCOMPARE(View->CaptureSelection(), Selection);
        QImage Changed(700, 280, QImage::Format_RGB32); Changed.fill(Qt::magenta); QVERIFY(Changed.save(Directory.filePath("photo00.png")));
        Model->Refresh(); QTRY_VERIFY(!Model->IsLoading() && Find("photo00.png") >= 0);
        QTRY_COMPARE(TileModel->index(Find("photo00.png"), 0).data(ThumbnailRole).value<QImage>().pixelColor(0, 0), QColor(Qt::magenta));
        auto* NameColumn = static_cast<Qtitan::GridTableColumn*>(View->BaseView()->getColumnByModelColumn(0));
        NameColumn->setSortOrder(Qtitan::SortDescending); QTest::qWait(160);
        for (int Row = 0; Row < TileModel->rowCount(); ++Row) QCOMPARE(TileModel->SourceIndex(Row), View->BaseView()->getRow(Row).modelIndex(0));
        NameColumn->setSortOrder(Qtitan::SortAscending); QTest::qWait(160);
        const auto Artifacts = qEnvironmentVariable("HC_UI_ARTIFACT_DIR");
        if (!Artifacts.isEmpty()) { QTest::qWait(200); QVERIFY(main_->grab().save(QDir(Artifacts).filePath("r4-main-thumbnail.png"))); }
        // Shared selection drives permanent deletion of our temporary fixture only.
        auto Delete = Bridge->Dispatch("hc.file.delete", {}, "test", TileModel->SourceIndex(Find("photo00.png")));
        QTRY_VERIFY(Delete.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready); const auto Deleted = Delete.get(); QVERIFY(Deleted.HasValue());
        QTRY_COMPARE(Scheduler.Snapshot(Deleted.Value().jobId_).Value().state_, JobState::WaitingForUser);
        auto* Confirm = main_->findChild<QPushButton*>("hc.jobs.choice." + QString::number(int(InteractionChoice::Confirm))); QVERIFY(Confirm); QTRY_VERIFY(Confirm->isVisible()); QTest::mouseClick(Confirm, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(IsTerminal(Scheduler.Snapshot(Deleted.Value().jobId_).Value().state_), 10000);
        QCOMPARE(Scheduler.Snapshot(Deleted.Value().jobId_).Value().state_, JobState::Completed); QVERIFY(!QFileInfo::exists(Directory.filePath("photo00.png"))); QVERIFY(QFileInfo::exists(Directory.filePath("photo01.png")));
        QTRY_VERIFY(!Model->IsLoading() && Find("folder") >= 0);
        Tiles->setCursorRow(Find("folder")); List->scrollTo(List->currentIndex()); QCoreApplication::processEvents();
        QSignalSpy Opened(Tiles, &ThumbnailView::activated);
        QTest::mouseClick(List->viewport(), Qt::LeftButton, Qt::NoModifier, List->visualRect(List->currentIndex()).center());
        QTest::mouseDClick(List->viewport(), Qt::LeftButton, Qt::NoModifier, List->visualRect(List->currentIndex()).center());
        QTRY_COMPARE(Opened.count(), 1);
        QTRY_VERIFY(!Model->IsLoading() && Panel->CurrentPath().endsWith("folder"));
        QTest::keyClick(List, Qt::Key_Backspace); QTRY_VERIFY(!Model->IsLoading() && names(Model).contains("photo01.png"));
        List->verticalScrollBar()->setValue(List->verticalScrollBar()->maximum()); QCoreApplication::processEvents();
        Panel->AddTab(true); QTRY_COMPARE(TileModel->Provider()->PendingCount(), 0); QCOMPARE(TileModel->Provider()->CacheCostKb(), 0);
        Panel->CloseTab(); QTRY_VERIFY(View->isVisible()); QVERIFY(View->IsThumbnailMode());
        auto Options = General["thumbnails"].toObject(); Options["autoSwitch"] = true; Options["autoPercent"] = 70; General["thumbnails"] = Options;
        QVERIFY(Settings.Put(hc::settings::Section::General, General)); View->ApplyDisplaySettings(); QVERIFY(View->SetListMode("auto")); QTRY_VERIFY(View->IsThumbnailMode());
        Panel->SetSearch("name:folder"); QTRY_VERIFY(!View->IsThumbnailMode()); QCOMPARE(View->ListMode(), QString("auto"));
        Panel->SetSearch({}); QTRY_VERIFY(View->IsThumbnailMode());
        QWheelEvent Wheel(QPointF(30, 30), List->mapToGlobal(QPoint(30, 30)), {}, QPoint(0, 120), Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(List->viewport(), &Wheel); QTRY_COMPARE(Tiles->appearance().size, 160);
        if (!Artifacts.isEmpty()) { QTest::qWait(100); QVERIFY(main_->grab().save(QDir(Artifacts).filePath("r4-main-thumbnail-large.png"))); }
    }

    void passwordManagerSettingsUi()
    {
        QTemporaryDir PasswordDirectory;
        const auto BeforePasswords = hc::passwords::RuntimeStore();
        const auto BeforeSettings = hc::settings::AppSettings().Object();
        auto& Theme = fm::style::ThemeManager::instance(); const auto Design = Theme.design(); const auto Scheme = Theme.scheme();
        auto Restore = qScopeGuard([&] {
            hc::passwords::RuntimeStore() = BeforePasswords; hc::passwords::ForgetSessionPasswords();
            hc::settings::AppSettings().DiscardEdit(); hc::settings::AppSettings().Replace(BeforeSettings); hc::settings::AppSettings().Commit();
            Theme.setDesign(Design); Theme.setScheme(Scheme);
        });
        hc::passwords::Store Passwords(PasswordDirectory.filePath("passwords")); QVERIFY(Passwords.Load());
        QVERIFY(Passwords.Add("개인 백업", "sample-password")); QVERIFY(Passwords.Add("공유 자료", "sample-other")); QVERIFY(Passwords.Add("이전 백업", "sample-old", false)); QVERIFY(Passwords.Save());
        hc::passwords::RuntimeStore() = Passwords;
        {
            QMainOpts Dialog(main_); Dialog.show();
            auto* Navigation = Dialog.findChild<QListWidget*>("lstOptCate"); QVERIFY(Navigation);
            int Row = -1; for (int Index = 0; Index < Navigation->count(); ++Index) if (Navigation->item(Index)->text() == "비밀번호 관리") Row = Index;
            QVERIFY(Row >= 0); Navigation->setCurrentRow(Row);
            auto* Page = Dialog.findChild<hc::passwords::SettingsPage*>(); QVERIFY(Page);
            QCOMPARE(Page->findChild<QTableWidget*>("hc.passwords.table")->rowCount(), 3);
            auto* Search = Dialog.findChild<QLineEdit*>("hc.settings.search"); QVERIFY(Search); Search->setText("비밀번호"); QVERIFY(!Navigation->item(Row)->isHidden()); Search->clear();
            const auto Out = qEnvironmentVariable("HC_UI_ARTIFACT_DIR");
            if (!Out.isEmpty()) {
                QVERIFY(QDir().mkpath(Out)); Theme.setDesign(fm::style::Design::Watercolor);
                for (bool Dark : {false, true}) { Theme.setScheme(Dark ? fm::style::ThemeManager::Scheme::Dark : fm::style::ThemeManager::Scheme::Light); QTest::qWait(100);
                    QVERIFY(Dialog.grab().save(QDir(Out).filePath(Dark ? "product-settings-dark.png" : "product-settings-light.png"))); }
            }
            Page->findChild<QCheckBox*>("hc.passwords.automatic")->setChecked(false); Dialog.reject();
        }
        QVERIFY(hc::passwords::RuntimeStore().Automatic());
        {
            QMainOpts Dialog(main_); Dialog.SetListView(qobject_cast<CGridView*>(panels_.front()->GetFocusView(panels_.front()->CurrentTabIndex())));
            Dialog.show(); QTest::qWait(80); Dialog.LoadSettings(); auto* Page = Dialog.findChild<hc::passwords::SettingsPage*>(); QVERIFY(Page);
            Page->findChild<QCheckBox*>("hc.passwords.automatic")->setChecked(false); QVERIFY(Dialog.SaveSettings()); Dialog.reject();
        }
        QVERIFY(!hc::passwords::RuntimeStore().Automatic());
        hc::passwords::Store Reloaded(Passwords.Path()); QVERIFY(Reloaded.Load()); QVERIFY(!Reloaded.Automatic()); QCOMPARE(Reloaded.Entries().size(), 3);
    }

    void r5SettingsUi()
    {
        auto& Settings = hc::settings::AppSettings(); const auto Original = Settings.Object(); const auto Workspace = main_->CaptureWorkspace();
        auto* Colors = TyStColorSchemeMgr::GetInstance(); const auto OriginalScheme = Colors->GetColorScheme(Colors->GetCurrentColorScheme());
        auto* Bridge = TyStCommandMgr::GetInstance()->Bridge();
        const auto Restore = qScopeGuard([&] { Settings.DiscardEdit(); Settings.Replace(Original); Settings.Commit(); Colors->Refresh(); emit Colors->sigColorSchemeChanged(OriginalScheme); TyStCommandMgr::GetInstance()->Refresh(); Bridge->RefreshFunctionBar(); main_->RestoreWorkspace(Workspace); });
        const auto Screenshot = [&](QMainOpts& Dialog, int Page, const QString& Name) { const auto Artifacts = qEnvironmentVariable("HC_UI_ARTIFACT_DIR"); if (Artifacts.isEmpty()) return true;
            Dialog.findChild<QListWidget*>("lstOptCate")->setCurrentRow(Page); QTest::qWait(80); return Dialog.grab().save(QDir(Artifacts).filePath(Name)); };
        auto General = Settings.Read(hc::settings::Section::General).toObject(); General["shortcutPreset"] = "legacy"; Settings.Put(hc::settings::Section::General, General);
        const auto CancelBaseline = Settings.Object(); const auto CancelAccent = fm::style::ThemeManager::instance().colors()[fm::style::Token::Accent];
        { QMainOpts Dialog(main_); Dialog.show(); QTest::qWait(80); Dialog.LoadSettings();
          auto* Theme = static_cast<hc::ui::ThemeSettingsPage*>(Dialog.findChild<QWidget*>("hc.settings.themeColors")); QVERIFY(Theme);
          Theme->SetAccent(QColor("#008577")); Dialog.findChild<QCheckBox*>("hc.settings.showCommandLine")->setChecked(false);
          Dialog.findChild<QComboBox*>("hc.settings.theme")->setCurrentIndex(1); Dialog.reject(); }
        QCOMPARE(Settings.Object(), CancelBaseline); QCOMPARE(fm::style::ThemeManager::instance().colors()[fm::style::Token::Accent], CancelAccent);
        General = Settings.Read(hc::settings::Section::General).toObject(); General["shortcutPreset"] = "legacy"; Settings.Put(hc::settings::Section::General, General);
        { QMainOpts Dialog(main_); auto* View = qobject_cast<CGridView*>(panels_.front()->GetFocusView(panels_.front()->CurrentTabIndex())); Dialog.SetListView(View); Dialog.show(); QTest::qWait(80); Dialog.LoadSettings();
          auto* Theme = static_cast<hc::ui::ThemeSettingsPage*>(Dialog.findChild<QWidget*>("hc.settings.themeColors"));
          const auto Before = Theme->Save(); auto Bad = Before; Bad["version"] = 99; QVERIFY(!Theme->Import(Bad)); QCOMPARE(Theme->Save(), Before);
          Bad = Before; Bad["accent"] = QJsonObject{{"color", "invalid"}}; QVERIFY(!Theme->Import(Bad)); QCOMPARE(Theme->Save(), Before);
          Dialog.findChild<QCheckBox*>("hc.theme.allTokens")->setChecked(true);
          auto* TokenTree = Dialog.findChild<QTreeWidget*>("hc.theme.tokenTree"); QVERIFY(TokenTree); QCOMPARE(TokenTree->topLevelItemCount(), 9);
          int TokenCount = 0; for (int I = 0; I < TokenTree->topLevelItemCount(); ++I) TokenCount += TokenTree->topLevelItem(I)->childCount(); QCOMPARE(TokenCount, 51);
          Dialog.findChild<QCheckBox*>("hc.theme.allTokens")->setChecked(false); QCOMPARE(Dialog.findChild<QListWidget*>("hc.theme.tokens")->count(), 11);
          auto* ThemeList = Dialog.findChild<QListWidget*>("hc.theme.tokens"); QVERIFY(!ThemeList->item(0)->icon().isNull());
          int IndividualColors = 0;
          for (const auto* Name : {"hc.theme.icons", "hc.theme.effects"}) {
              auto* Button = Theme->findChild<QPushButton*>(Name); QVERIFY(Button && Button->menu());
              for (auto* Action : Button->menu()->actions()) { Action->trigger(); QCOMPARE(TokenTree->currentItem()->data(0, Qt::UserRole).toInt(), Action->data().toInt()); ++IndividualColors; }
          }
          QCOMPARE(IndividualColors, 12); Dialog.findChild<QCheckBox*>("hc.theme.allTokens")->setChecked(false);
          {
              Dialog.findChild<QCheckBox*>("hc.theme.allTokens")->setChecked(true); auto* Find = Theme->findChild<QLineEdit*>("hc.theme.search");
              Find->setText("--k-doc"); QVERIFY(TokenTree->currentItem()); QCOMPARE(TokenTree->currentItem()->data(0, Qt::UserRole).toInt(), int(fm::style::Token::KDoc));
              Theme->findChild<fm::ui::ColorPickButton*>("hc.theme.color")->colorChanged(QColor("#123456")); const auto Edited = Theme->Save();
              QCOMPARE(TokenTree->currentItem()->data(0, Qt::UserRole).toInt(), int(fm::style::Token::KDoc));
              Find->setText("no-such-token"); QVERIFY(!Theme->findChild<QWidget*>("hc.theme.editor")->isEnabled()); QCOMPARE(Theme->Save(), Edited);
              Find->clear(); Theme->findChild<QPushButton*>("hc.theme.filter1")->click(); Find->setText("--k-doc"); QVERIFY(TokenTree->currentItem());
              QCOMPARE(TokenTree->currentItem()->data(0, Qt::UserRole).toInt(), int(fm::style::Token::KDoc)); QCOMPARE(Theme->Save(), Edited);
              Theme->findChild<QPushButton*>("hc.theme.filter0")->click(); Find->clear(); Dialog.findChild<QCheckBox*>("hc.theme.allTokens")->setChecked(false); Theme->Load(Before);
          }
          {
              using namespace fm::style; Theme->Load({}); Theme->SetAccent(QColor("#123456"));
              auto* VariantChoice = Theme->findChild<QComboBox*>("hc.theme.variant");
              auto* Swatches = Theme->findChild<fm::ui::AccentPicker*>(); VariantChoice->setCurrentIndex(0); ThemeList->setCurrentRow(int(SeedRole::Accent));
              Swatches->accentChosen(QColor("#AA2233"));
              QCOMPARE(Theme->Scheme().seeds.accent.value(), QColor("#123456"));
              QCOMPARE(Theme->Scheme().seeds.variantSeeds(Design::Watercolor, Variant::Light).seed[0].value(), QColor("#AA2233"));
              QVERIFY(!Theme->Scheme().seeds.variantSeeds(Design::Watercolor, Variant::Dark).seed[0]);
              VariantChoice->setCurrentIndex(1); Swatches->accentChosen(QColor("#3344BB"));
              QCOMPARE(Theme->Scheme().seeds.variantSeeds(Design::Watercolor, Variant::Dark).seed[0].value(), QColor("#3344BB"));
              auto* Color = Theme->findChild<fm::ui::ColorPickButton*>("hc.theme.color");
              ThemeList->setCurrentRow(int(SeedRole::Sel)); Color->colorChanged(QColor("#225522"));
              QVERIFY(!Theme->Scheme().seeds.variantSeeds(Design::Watercolor, Variant::Dark).selFollowsAccent);
              const auto Derived = deriveTheme(Variant::Dark, Theme->Scheme().seeds, {}, Design::Watercolor).colors;
              QCOMPARE(Derived[Token::Sel], QColor("#225522"));
              Theme->findChild<QPushButton*>("hc.theme.resetColor")->click(); QVERIFY(Theme->Scheme().seeds.variantSeeds(Design::Watercolor, Variant::Dark).selFollowsAccent);
              ThemeList->setCurrentRow(int(SeedRole::Accent)); Theme->findChild<QPushButton*>("hc.theme.resetColor")->click();
              QVERIFY(!Theme->Scheme().seeds.variantSeeds(Design::Watercolor, Variant::Dark).seed[0]);
              Theme->SetSystemAccent(true); auto Effective = Theme->Scheme().seeds; if (auto System = ThemeManager::systemAccent()) Effective.accent = *System;
              const auto Expected = seedColor(SeedRole::Accent, deriveTheme(Variant::Dark, Effective, {}, Design::Watercolor).colors, Effective);
              QCOMPARE(Theme->findChild<QLineEdit*>("hc.theme.hex")->text(), colorHex(Expected));
              VariantChoice->setCurrentIndex(0); Theme->Load(Before);
          }
          const auto OriginalLibrary = Settings.Read(hc::settings::Section::Appearance).toObject()["watercolorPresets"].toObject();
          {
              using namespace fm::style; ColorScheme Fixture; auto& Overrides = Fixture.overridesFor(Design::Watercolor, Variant::Light);
              Overrides[indexOf(Token::Surface)] = QColor(Qt::white); Overrides[indexOf(Token::Fg)] = QColor(Qt::white);
              Theme->Load(Fixture.toJson()); auto* Contrast = Theme->findChild<QTableWidget*>("hc.theme.contrastTable"); QVERIFY(Contrast); QCOMPARE(Contrast->rowCount(), 9);
              QCOMPARE(Contrast->item(0, 1)->text(), QString("1.0:1")); QVERIFY(Contrast->item(0, 2)->text().contains(QString::fromUtf8("낮음")));
              QVERIFY(Theme->findChild<QCheckBox*>("hc.theme.fixContrast")->isChecked());
              QVERIFY(Screenshot(Dialog, 1, "theme-contrast-warning.png"));
              auto* Preview = Theme->findChild<QWidget*>("hc.theme.preview"); QVERIFY(Preview); const auto Sample = Preview->grab().toImage();
              const auto Dpr = Preview->devicePixelRatioF(); QCOMPARE(Sample.pixelColor(int(3 * Dpr), int((Preview->fontMetrics().height() + 11) * Dpr)), QColor(Qt::white));
              for (auto* Scroll : Theme->findChildren<QScrollArea*>()) Scroll->verticalScrollBar()->setValue(Scroll->verticalScrollBar()->maximum());
              QVERIFY(Screenshot(Dialog, 1, "theme-preview-contrast.png"));
              Theme->Load(Before); for (auto* Scroll : Theme->findChildren<QScrollArea*>()) Scroll->verticalScrollBar()->setValue(0);
          }
          {
              using namespace fm::style; ColorScheme Fixture; Fixture.seeds.accent = QColor("#123456");
              Fixture.seeds.variantSeeds(Design::Watercolor, Variant::Light).seed[std::size_t(SeedRole::Win)] = QColor("#EEEEEE");
              Fixture.seeds.variantSeeds(Design::Watercolor, Variant::Dark).seed[std::size_t(SeedRole::Win)] = QColor("#222222");
              Theme->Load(Fixture.toJson()); QVERIFY(Theme->findChild<QComboBox*>("hc.theme.preset")->currentText().contains(QString::fromUtf8("수정됨")));
              const auto Name = QString("theme-test-") + QUuid::createUuid().toString(); QVERIFY(Theme->SavePresetAs(Name)); QVERIFY(!Theme->SavePresetAs(Name));
              const auto SavedId = Theme->Scheme().id; Theme->SetAccent(QColor("#654321")); auto* Presets = Theme->findChild<QComboBox*>("hc.theme.preset");
              QVERIFY(Presets->currentText().contains(QString::fromUtf8("수정됨")));
              QVERIFY(QMetaObject::invokeMethod(Presets, "activated", Q_ARG(int, Presets->findData("builtin")))); QCOMPARE(Theme->Scheme().seeds.accent.value(), QColor("#123456"));
              QVERIFY(QMetaObject::invokeMethod(Presets, "activated", Q_ARG(int, Presets->findData(SavedId)))); QCOMPARE(Theme->Scheme().seeds.accent.value(), QColor("#654321"));
              QTemporaryDir Files; QVERIFY(Files.isValid()); const auto Exported = Files.filePath("theme.json"); QVERIFY(Theme->Export(Exported));
              hc::ui::ThemeSettingsPage Reload; QVERIFY(Reload.ImportFile(Exported)); QCOMPARE(Reload.Save(), Theme->Save());
              const auto Stable = Reload.Save(); QFile Invalid(Files.filePath("invalid.json")); QVERIFY(Invalid.open(QIODevice::WriteOnly)); Invalid.write("not json"); Invalid.close();
              QVERIFY(!Reload.ImportFile(Invalid.fileName())); QCOMPARE(Reload.Save(), Stable); QVERIFY(!Theme->Export(Settings.FileName()));
              Theme->findChild<QPushButton*>("hc.theme.reset")->click();
              QVERIFY(!Theme->Scheme().seeds.variantSeeds(Design::Watercolor, Variant::Light).seed[std::size_t(SeedRole::Win)]);
              QCOMPARE(Theme->Scheme().seeds.variantSeeds(Design::Watercolor, Variant::Dark).seed[std::size_t(SeedRole::Win)].value(), QColor("#222222"));
              QCOMPARE(Theme->Scheme().seeds.accent.value(), QColor("#654321")); Theme->LoadLibrary(OriginalLibrary); Theme->Load(Before);
          }
          auto PresetA = Before; PresetA["id"] = "hc.test.a"; PresetA["name"] = "테스트 A";
          auto PresetB = Before; PresetB["id"] = "hc.test.b"; PresetB["name"] = "테스트 B";
          const QJsonObject FuturePreset{{"version", 99}, {"metadata", "kept"}};
          Theme->LoadLibrary({{"hc.test.a", PresetA}, {"hc.test.b", PresetB}, {"future", FuturePreset}}); Theme->Load(PresetA); Theme->SetAccent(QColor("#123456"));
          auto* Presets = Dialog.findChild<QComboBox*>("hc.theme.preset"); QVERIFY(QMetaObject::invokeMethod(Presets, "activated", Q_ARG(int, Presets->findData("hc.test.b"))));
          QCOMPARE(Theme->SaveLibrary()["hc.test.a"].toObject()["accent"].toObject()["color"].toString(), QString("#123456")); QCOMPARE(Theme->SaveLibrary()["future"].toObject(), FuturePreset);
          QVERIFY(QMetaObject::invokeMethod(Presets, "activated", Q_ARG(int, Presets->findData("hc.test.a")))); QCOMPARE(Theme->Scheme().seeds.accent.value(), QColor("#123456"));
          Theme->LoadLibrary(OriginalLibrary); Theme->Load(Before);
          auto Rich = Before; Rich["metadata"] = QJsonObject{{"kept", true}}; Rich["designs"] = QJsonObject{{"watercolor", QJsonObject{{"light", QJsonObject{{"metadata", "retained"}, {"overrides", QJsonObject{{"--k-doc", "#112233"}}}}}}}};
          QVERIFY(Theme->Import(Rich)); QCOMPARE(Theme->Save()["metadata"].toObject()["kept"].toBool(), true);
          Dialog.findChild<QPushButton*>("hc.theme.reset")->click(); QVERIFY(!Theme->Save()["designs"].toObject()["watercolor"].toObject()["light"].toObject()["overrides"].toObject().contains("--k-doc")); QCOMPARE(Theme->Save()["designs"].toObject()["watercolor"].toObject()["light"].toObject()["metadata"].toString(), QString("retained"));
          Theme->SetAccent(QColor("#008577"));
          auto* Keys = static_cast<hc::ui::KeyboardSettingsPage*>(Dialog.findChild<QWidget*>("hc.settings.keyboard")); QVERIFY(Keys);
          auto* Selector = static_cast<hc::ui::CommandSelector*>(Dialog.findChild<QComboBox*>("hc.keys.command")); Selector->SetCommand("hc.file.copy");
          auto* Search = Dialog.findChild<QLineEdit*>("hc.keys.search"); Search->setText("hc.file.copy"); auto* Table = Dialog.findChild<QTableWidget*>("hc.keys.commands"); int Visible = 0; for (int I = 0; I < Table->rowCount(); ++I) if (!Table->isRowHidden(I)) ++Visible; QCOMPARE(Visible, 1); Search->clear();
          QVERIFY(Screenshot(Dialog, 7, "r5-keyboard-light.png"));
          auto* Capture = Dialog.findChild<fm::ui::KeyCaptureEdit*>("hc.keys.capture"); Dialog.findChild<QPushButton*>("hc.keys.listen")->click(); QTest::keyClick(Capture, Qt::Key_M, Qt::ControlModifier);
          QCOMPARE(Capture->pending(), QKeySequence("Ctrl+M")); QVERIFY(!Dialog.findChild<QPushButton*>("hc.keys.assign")->isEnabled()); QVERIFY(!Dialog.findChild<QPushButton*>("hc.keys.reassign")->isHidden());
          Dialog.findChild<QPushButton*>("hc.keys.reassign")->click(); QCOMPARE(hc::ui::CommandId(TyStCommandMgr::GetInstance()->Retrieve().value(QKeySequence("Ctrl+M")[0])), QString("hc.file.copy"));
          auto* F = static_cast<hc::ui::CommandSelector*>(Dialog.findChild<QComboBox*>("hc.keys.function3")); F->SetCommand("hc.navigate.refresh");
          Dialog.findChild<QComboBox*>("hc.settings.theme")->setCurrentIndex(0);
          Dialog.findChild<QCheckBox*>("hc.settings.showCommandLine")->setChecked(false); Dialog.findChild<QComboBox*>("hc.settings.startup")->setCurrentIndex(1);
          QVERIFY(Dialog.SaveSettings()); QCOMPARE(fm::style::ThemeManager::instance().colors()[fm::style::Token::Accent], QColor("#008577")); QVERIFY(main_->findChild<QWidget*>("hc.commandLine")->isHidden());
          auto* Bar = main_->findChild<fm::ui::FunctionKeyBar*>("hc.functionBar"); QVERIFY(Bar); QCOMPARE(Bar->buttons()[3]->property("hc.commandId").toString(), QString("hc.navigate.refresh"));
          QVERIFY(Screenshot(Dialog, 0, "r5-general-light.png")); QVERIFY(Screenshot(Dialog, 1, "r5-theme-light.png")); QVERIFY(Screenshot(Dialog, 2, "r5-panel-light.png")); QVERIFY(Screenshot(Dialog, 3, "r5-thumbnail-light.png")); auto* RulePage = static_cast<hc::ui::FileColorRulesPage*>(Dialog.findChild<QWidget*>("hc.settings.fileColors")); const auto RuleBefore = RulePage->Document();
          auto RuleFixture = hc::settings::FileColorRules::Empty(); RuleFixture["groups"] = QJsonArray{QJsonObject{{"id", "r5.source"}, {"name", "소스 파일"}, {"enabled", true}, {"join", "any"}, {"conditions", QJsonArray{QJsonObject{{"type", "extension"}, {"value", "cpp, hpp"}}}}, {"light", QJsonObject{{"foreground", "#008577"}}}, {"dark", QJsonObject{{"foreground", "#69D5BE"}}}}};
          RulePage->Load(RuleFixture); RulePage->findChild<QLineEdit*>("hc.rules.sample")->setText("mainwindow.cpp"); QVERIFY(Screenshot(Dialog, 4, "r5-rules-light.png")); RulePage->Load(RuleBefore); QVERIFY(Screenshot(Dialog, 5, "r5-columns-light.png"));
          auto* Categories = Dialog.findChild<QListWidget*>("lstOptCate"); Dialog.findChild<QLineEdit*>("hc.settings.search")->setText("캐시"); QVERIFY(Categories->item(3)->isHidden() == false); Dialog.findChild<QLineEdit*>("hc.settings.search")->clear();
          Dialog.findChild<QComboBox*>("hc.settings.theme")->setCurrentIndex(1); QVERIFY(Dialog.SaveSettings()); Dialog.findChild<QComboBox*>("hc.theme.variant")->setCurrentIndex(1);
          QVERIFY(Screenshot(Dialog, 0, "r5-general-dark.png")); QVERIFY(Screenshot(Dialog, 1, "r5-theme-dark.png")); QVERIFY(Screenshot(Dialog, 7, "r5-keyboard-dark.png"));
          {
              Dialog.findChild<QListWidget*>("lstOptCate")->setCurrentRow(1); Dialog.raise(); Dialog.activateWindow(); QTRY_VERIFY(Dialog.isActiveWindow());
              auto* Tokens = Theme->findChild<QPushButton*>("hc.theme.tokensMode"); Tokens->setFocus(); QTest::keyClick(Tokens, Qt::Key_Space);
              QVERIFY(Theme->findChild<QCheckBox*>("hc.theme.allTokens")->isChecked()); Theme->findChild<QLineEdit*>("hc.theme.search")->setText("강조");
              const auto Full = Dialog.size(); auto* Sidebar = Dialog.findChild<QWidget*>("hc.settings.sidebar"); const int SideWidth = Sidebar->width();
              Dialog.resize(Full.width() + 180, Full.height()); QTest::qWait(40); QCOMPARE(Sidebar->width(), SideWidth); QVERIFY(Screenshot(Dialog, 1, "theme-all-wide.png"));
              Dialog.resize(900, 650); QTest::qWait(40); const auto Small = Dialog.size();
              for (auto* Scroll : Theme->findChildren<QScrollArea*>()) QCOMPARE(Scroll->horizontalScrollBar()->maximum(), 0);
              for (int I = 0; I < 4; ++I) { Dialog.move(Dialog.pos() + QPoint(I % 2 ? -8 : 8, 0)); QCoreApplication::processEvents(); QCOMPARE(Dialog.size(), Small); }
              QVERIFY(Screenshot(Dialog, 1, "theme-all-small.png")); Dialog.resize(Full); QTest::qWait(40); QCOMPARE(Dialog.size(), Full);
              Theme->findChild<QLineEdit*>("hc.theme.search")->clear(); TokenTree->setFocus(); auto* FirstItem = TokenTree->currentItem(); QTest::keyClick(TokenTree, Qt::Key_Down); QVERIFY(TokenTree->currentItem() != FirstItem);
              Theme->findChild<QPushButton*>("hc.theme.seedsMode")->click();
              for (auto* Scroll : Theme->findChildren<QScrollArea*>()) { Scroll->verticalScrollBar()->setValue(Scroll->verticalScrollBar()->maximum()); QCoreApplication::processEvents(); }
              QVERIFY(Screenshot(Dialog, 1, "theme-dark-preview-bottom.png"));
              const auto Artifacts = qEnvironmentVariable("HC_UI_ARTIFACT_DIR"); if (!Artifacts.isEmpty()) { QFile Dpi(QDir(Artifacts).filePath("theme-dpi.txt")); QVERIFY(Dpi.open(QIODevice::WriteOnly)); Dpi.write(QByteArray::number(Dialog.devicePixelRatioF())); }
          }
          Dialog.findChild<QComboBox*>("hc.settings.theme")->setCurrentIndex(2); QVERIFY(Dialog.SaveSettings()); QCOMPARE(fm::style::ThemeManager::instance().scheme(), fm::style::ThemeManager::Scheme::System); Dialog.reject(); }
        QProcess Restart; auto Environment = QProcessEnvironment::systemEnvironment(); Environment.insert("HC_R5_RESTART", "1"); Environment.insert("HC_TEST_CONFIG_DIR", QFileInfo(Settings.FileName()).absolutePath()); Restart.setProcessEnvironment(Environment);
        Restart.start(QCoreApplication::applicationFilePath(), {"r5RestartUi", "-o", "r5-restart.txt,txt"}); QVERIFY(Restart.waitForStarted(5000)); QVERIFY(Restart.waitForFinished(30000)); QCOMPARE(Restart.exitCode(), 0);
        { hc::ui::ThemeSettingsPage Page; const QJsonObject Future{{"format", "fm-color-scheme"}, {"version", 99}, {"future", "preserve"}}; Page.Load(Future); QVERIFY(!Page.isEnabled()); QCOMPARE(Page.Save(), Future); }
        // Migration is an explicit draft action, keeps source bytes, stable IDs
        // and independently proves attribute/size preview inputs.
        const QJsonArray Legacy{QJsonObject{{"name", "R5 소스"}, {"flags", FILE_SET_NOR_EXT_FILTERS | FILE_SET_EXP_ATTRIBUTE}, {"extensions", QJsonArray{"cpp", "hpp"}}, {"attributes", 32}, {"filters", QJsonArray{}}, {"size", 0}}}; Settings.Put(hc::settings::Section::FileSets, Legacy);
        { hc::ui::FileColorRulesPage Page; Page.ImportLegacy(); const auto Doc = Page.Document(); QVERIFY(Doc.contains("legacyBackup")); QCOMPARE(Doc["legacyBackup"].toObject()["fileSets"].toArray(), Legacy); const auto Groups = Doc["groups"].toArray(); QCOMPARE(Groups.size(), 1); Page.ImportLegacy(); QCOMPARE(Page.Document()["groups"].toArray(), Groups);
          hc::settings::FileColorRules Rules; Rules.Load(Doc); QVERIFY(Rules.Evaluate("sample.cpp", 32, 1, false).matched_); QVERIFY(!Rules.Evaluate("sample.cpp", 0, 1, false).matched_); QVERIFY(!Rules.Evaluate("sample.txt", 32, 1, false).matched_); }
    }

    void r5RestartUi()
    {
        if (!qEnvironmentVariableIsSet("HC_R5_RESTART")) QSKIP("Only runs as a settings restart child");
        auto& Settings = hc::settings::AppSettings(); const auto G = Settings.Read(hc::settings::Section::General).toObject(); QCOMPARE(G["themeMode"].toString(), QString("system")); QVERIFY(!G["restoreWorkspace"].toBool());
        QCOMPARE(fm::style::ThemeManager::instance().scheme(), fm::style::ThemeManager::Scheme::System); QCOMPARE(hc::ui::FunctionCommands(G)[3], QString("hc.navigate.refresh"));
        QCOMPARE(hc::ui::CommandId(TyStCommandMgr::GetInstance()->Retrieve().value(QKeySequence("Ctrl+M")[0])), QString("hc.file.copy"));
        const auto S = fm::style::ColorScheme::fromJson(Settings.Read(hc::settings::Section::Appearance).toObject()["watercolorTheme"].toObject()); QVERIFY(S); QCOMPARE(S->seeds.accent.value(), QColor("#008577")); QVERIFY(main_->findChild<QWidget*>("hc.commandLine")->isHidden());
    }

    void todoBugQuickFindSettings()
    {
        auto& Settings = hc::settings::AppSettings(); const auto Before = Settings.Object();
        const auto Restore = qScopeGuard([&] { Settings.DiscardEdit(); Settings.Replace(Before); Settings.Commit(); });
        QMainOpts Dialog(main_); Dialog.show(); QTest::qWait(50); Dialog.LoadSettings();
        auto* Page = Dialog.findChild<QWidget*>("hc.settings.navigation"); QVERIFY(Page);
        auto* Tabs = Page->findChild<QTabWidget*>("hc.navigation.tabs"); QVERIFY(Tabs);
        int QuickTab = -1; for (int I = 0; I < Tabs->count(); ++I) if (Tabs->tabText(I) == "빠른 찾기") QuickTab = I;
        QVERIFY(QuickTab >= 0); Tabs->setCurrentIndex(QuickTab);
        auto* Categories = Dialog.findChild<QListWidget*>("lstOptCate"); QVERIFY(Categories);
        for (int I = 0; I < Categories->count(); ++I) if (Categories->item(I)->text() == "탐색") Categories->setCurrentRow(I);
        auto* Enabled = Page->findChild<QCheckBox*>("hc.navigation.quickFind.enabled"); QVERIFY(Enabled);
        auto* Activation = Page->findChild<QComboBox*>("hc.navigation.quickFind.activation"); QVERIFY(Activation);
        auto* Scope = Page->findChild<QComboBox*>("hc.navigation.quickFind.scope"); QVERIFY(Scope);
        auto* Match = Page->findChild<QComboBox*>("hc.navigation.quickFind.match"); QVERIFY(Match);
        auto* Result = Page->findChild<QComboBox*>("hc.navigation.quickFind.result"); QVERIFY(Result);
        auto* Highlight = Page->findChild<QCheckBox*>("hc.navigation.quickFind.highlight"); QVERIFY(Highlight);
        auto* DisplayInterval = Page->findChild<QSpinBox*>("hc.navigation.quickFind.everythingDisplayIntervalMs"); QVERIFY(DisplayInterval);
        DisplayInterval->setValue(750);
        QVERIFY(hc::quickfind::Settings::FromJson({}).highlight_); QVERIFY(Highlight->isChecked());
        QCOMPARE(hc::quickfind::Settings::FromJson({}).result_, QString("cursor"));
        QCOMPARE(Result->currentData().toString(), QString("cursor"));
        Enabled->setChecked(false); QVERIFY(!Activation->isEnabled() && !Scope->isEnabled() && !Match->isEnabled() && !Result->isEnabled());
        QVERIFY(DisplayInterval->isEnabled());
        Enabled->setChecked(true); QVERIFY(Activation->isEnabled());
        Activation->setCurrentIndex(Activation->findData("typing")); Scope->setCurrentIndex(Scope->findData("everything")); Match->setCurrentIndex(Match->findData("ends"));
        Result->setCurrentIndex(Result->findData("filter"));
        Highlight->setChecked(false);
        QVERIFY(SaveR9Dialog(Dialog, "quick-find-settings"));
        QVERIFY(Dialog.SaveSettings()); Dialog.reject();
        const auto Saved = Settings.Read(hc::settings::Section::General).toObject()["quickFind"].toObject();
        QCOMPARE(Saved["everythingDisplayIntervalMs"].toInt(), 750);
        QCOMPARE(Saved["activation"].toString(), QString("typing")); QCOMPARE(Saved["scope"].toString(), QString("everything")); QCOMPARE(Saved["match"].toString(), QString("ends"));
        QMainOpts Reopened(main_); Reopened.LoadSettings();
        QCOMPARE(Reopened.findChild<QSpinBox*>("hc.navigation.quickFind.everythingDisplayIntervalMs")->value(), 750);
        QVERIFY(!Reopened.findChild<QCheckBox*>("hc.navigation.quickFind.highlight")->isChecked());
        QCOMPARE(Reopened.findChild<QComboBox*>("hc.navigation.quickFind.activation")->currentData().toString(), QString("typing"));
        QCOMPARE(Reopened.findChild<QComboBox*>("hc.navigation.quickFind.scope")->currentData().toString(), QString("everything"));
        QCOMPARE(Reopened.findChild<QComboBox*>("hc.navigation.quickFind.result")->currentData().toString(), QString("filter"));
        QCOMPARE(Reopened.findChild<QComboBox*>("hc.navigation.quickFind.match")->currentData().toString(), QString("ends")); Reopened.reject();
    }

    void todoBugQuickFindInput()
    {
        auto& Settings = hc::settings::AppSettings(); const auto Before = Settings.Object(); const auto Workspace = main_->CaptureWorkspace();
        auto* Find = main_->findChild<QLineEdit*>("hc.mainFind"); QVERIFY(Find);
        const auto Restore = qScopeGuard([&] { Find->clear(); Settings.Replace(Before); Settings.Commit(); main_->RestoreWorkspace(Workspace); });
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        for (const auto& Name : {QString("alpha.txt"), QString("alphabet.log"), QString("my-alpha.txt"), QString("space alpha.txt"), QString("한글 파일.txt"), QString("notes 한글.txt")}) {
            QFile File(Directory.filePath(Name)); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("quick find");
        }
        auto* Panel = panels_.front(); Panel->ChangeCurrentPath(Directory.path(), {}); QTRY_VERIFY(!model(Panel)->IsLoading());
        auto* View = static_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex()));
        const auto Configure = [&](bool Enabled, const QString& Activation, const QString& Match) {
            auto General = Settings.Read(hc::settings::Section::General).toObject(); General["quickFind"] = QJsonObject{{"enabled", Enabled}, {"activation", Activation}, {"scope", "panel"}, {"match", Match}, {"result", "filter"}};
            return Settings.Put(hc::settings::Section::General, General);
        };
        const auto Focus = [&] { TyStCommandMgr::GetInstance()->Bridge()->ExecuteCommand("hc.search.focus"); Find->clear(); View->EnsureKeyboardFocusOnView(); QTest::qWait(30); return QApplication::focusWidget(); };
        QVERIFY(Configure(false, "typing", "starts")); auto* Target = Focus(); QVERIFY(Target && Target != Find);
        QTest::keyClick(Target, Qt::Key_A); QVERIFY(!Find->hasFocus()); QVERIFY(Find->text().isEmpty());
        QVERIFY(Configure(true, "ctrl-alt", "starts")); Target = Focus();
        QTest::keyClick(Target, Qt::Key_A); QVERIFY(!Find->hasFocus());
        QTest::keyClick(Target, Qt::Key_A, Qt::ControlModifier | Qt::AltModifier); QTRY_VERIFY(Find->hasFocus()); QCOMPARE(Find->text(), QString("a"));
        QTest::keyClicks(Find, "lpha"); QCOMPARE(Find->text(), QString("alpha")); QCOMPARE(Panel->SearchMatch(), QString("starts")); QTRY_COMPARE(View->BaseView()->getRowCount(), 3);
        QTest::keyClick(Find, Qt::Key_Escape); QVERIFY(Find->text().isEmpty()); QVERIFY(!Find->hasFocus());
        for (const auto& Match : {QString("starts"), QString("contains"), QString("ends")}) {
            QVERIFY(Configure(true, "typing", Match)); Target = Focus();
            QTest::keyClick(Target, Qt::Key_A); QTRY_VERIFY(Find->hasFocus()); QTest::keyClicks(Find, "lpha");
            QTRY_COMPARE(View->BaseView()->getRowCount(), Match == "starts" ? 3 : Match == "contains" ? 5 : 1);
            QCOMPARE(Panel->SearchMatch(), Match);
        }
        QVERIFY(Configure(true, "typing", "starts")); Target = Focus();
        QInputMethodEvent Initial(QStringLiteral("ㅎ"), {}); QApplication::sendEvent(Target, &Initial);
        QTRY_VERIFY(Find->hasFocus()); QCOMPARE(Find->text(), QString{});
        QInputMethodEvent Syllable(QStringLiteral("한"), {}); QApplication::sendEvent(Find, &Syllable); QCOMPARE(Find->text(), QString{});
        QInputMethodEvent Next(QStringLiteral("ㄱ"), {}); Next.setCommitString(QStringLiteral("한")); QApplication::sendEvent(Find, &Next);
        QCOMPARE(Find->text(), QStringLiteral("한"));
        QInputMethodEvent Finish; Finish.setCommitString(QStringLiteral("글")); QApplication::sendEvent(Find, &Finish);
        QTRY_COMPARE(Find->text(), QStringLiteral("한글")); QTRY_COMPARE(View->BaseView()->getRowCount(), 2);
        QVERIFY(!Find->text().contains(QChar(0x314e))); // No detached initial consonant.
        QInputMethodEvent Replace; Replace.setCommitString(QStringLiteral("국"), -1, 1); QApplication::sendEvent(Find, &Replace);
        QCOMPARE(Find->text(), QStringLiteral("한국"));
        QTest::keyClick(Find, Qt::Key_Backspace); QCOMPARE(Find->text(), QStringLiteral("한"));
        const auto Nfd = QStringLiteral("한글").normalized(QString::NormalizationForm_D);
        QVERIFY(hc::quickfind::Matches(QStringLiteral("한글 파일.txt"), Nfd, "starts"));
        QVERIFY(hc::quickfind::Matches(QStringLiteral("notes 한글.txt"), "한글.txt", "ends"));
        Target = Focus(); QTest::keyClick(Target, Qt::Key_C, Qt::ControlModifier); QVERIFY(!Find->hasFocus());
        TyStCommandMgr::GetInstance()->Bridge()->ExecuteCommand("hc.search.focus"); QTest::keyClicks(Find, "ext:log"); QTRY_COMPARE(View->BaseView()->getRowCount(), 2); QVERIFY(Panel->SearchMatch().isEmpty());
    }

    void everythingPanelStreamingUi()
    {
        auto& Settings = hc::settings::AppSettings(); const auto Before = Settings.Object(); const auto Workspace = main_->CaptureWorkspace();
        auto* Find = main_->findChild<QLineEdit*>("hc.mainFind"); QVERIFY(Find);
        const auto Restore = qScopeGuard([&] {
            Find->clear(); qunsetenv("HC_EVERYTHING_TEST_INPUT"); qunsetenv("HC_EVERYTHING_TEST_MODE");
            Settings.Replace(Before); Settings.Commit(); main_->RestoreWorkspace(Workspace);
        });
        QTemporaryDir Dir; QVERIFY(Dir.isValid()); QDir Root(Dir.path()); QVERIFY(Root.mkpath("origin")); QVERIFY(Root.mkpath("a")); QVERIFY(Root.mkpath("b"));
        QJsonArray Paths;
        const auto Anchor = Dir.filePath("origin/hc-everything-anchor.txt");
        { QFile File(Anchor); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("anchor"); } Paths.append(Anchor);
        for (int I = 0; I < 301; ++I) {
            const auto Name = I < 2 ? QString("hc-everything-same.txt") : QString("hc-everything-%1.txt").arg(I);
            const auto Path = Dir.filePath((I % 2 ? "a/" : "b/") + Name);
            QFile File(Path); QVERIFY(File.open(QIODevice::WriteOnly)); File.write(QByteArray("data-") + QByteArray::number(I)); Paths.append(Path);
        }
        QFile Fixture(Dir.filePath("input.json")); QVERIFY(Fixture.open(QIODevice::WriteOnly)); Fixture.write(QJsonDocument(Paths).toJson()); Fixture.close();
        qputenv("HC_EVERYTHING_TEST_INPUT", Fixture.fileName().toUtf8()); qputenv("HC_EVERYTHING_TEST_MODE", "slow");
        auto General = Settings.Read(hc::settings::Section::General).toObject();
        General["quickFind"] = QJsonObject{{"enabled", false}, {"scope", "everything"}, {"match", "starts"}, {"everythingDisplayIntervalMs", 50}};
        QVERIFY(Settings.Put(hc::settings::Section::General, General)); QVERIFY(Settings.Commit());
        auto* Panel = panels_.front(); Panel->ChangeCurrentPath(Dir.filePath("origin"), {});
        QTRY_VERIFY(!model(Panel)->IsLoading());
        auto* Model = static_cast<CFSModel*>(model(Panel)); auto* View = static_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex()));
        const auto AnchorKey = hc::workspace::SelectionKey(*Model->GetFileInfo(Model->index(1, 0))->storageEntry_);
        const auto OriginalColor = Model->index(1, 0).data(Qt::ForegroundRole); View->RestoreSelection({AnchorKey}, AnchorKey);
        QVERIFY(View->SetListMode("one-line"));
        const auto OriginalPresentation = View->CaptureWorkspaceView().value("listPresentation");
        Model->setProperty("hc.search.helper", QCoreApplication::applicationFilePath());
        main_->raise(); main_->activateWindow(); QVERIFY(QTest::qWaitForWindowActive(main_));
        View->EnsureKeyboardFocusOnView(); emit Panel->sig_NotifyPanelActivated();
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_F, Qt::ControlModifier); QTRY_VERIFY(Find->hasFocus());
        Find->setText("hc-everything");
        auto* Progress = Find->findChild<QWidget*>("hc.quickFind.progress"); QVERIFY(Progress);
        QVERIFY(Progress->isVisible()); QCOMPARE(Progress->height(), 2); QVERIFY(Find->property("hc.quickFind.busy").toBool());
        QTRY_VERIFY(Model->IsSearchResults());
        QTRY_VERIFY_WITH_TIMEOUT(Model->rowCount() > 0, 6000); QVERIFY(Model->rowCount() < 302);
        QVERIFY(Progress->isVisible());
        const auto BeforeProgress = Progress->grab().toImage(); QTest::qWait(90);
        QVERIFY(BeforeProgress != Progress->grab().toImage());
        const auto Capture = qEnvironmentVariable("HC_EVERYTHING_CAPTURE_ROOT");
        if (!Capture.isEmpty()) { QDir().mkpath(Capture); QVERIFY(main_->grab().save(QDir(Capture).filePath("everything-loading.png"))); QVERIFY(Find->grab().save(QDir(Capture).filePath("everything-loading-input.png"))); }
        QSignalSpy AppendResets(Model, &QAbstractItemModel::modelReset);
        View->RestoreSelection({AnchorKey}, AnchorKey);
        QTRY_COMPARE_WITH_TIMEOUT(Model->rowCount(), 302, 15000); QVERIFY(!Model->UsesIndexedOrder()); QCOMPARE(Model->EntryCount(), quint64(302));
        QCOMPARE(AppendResets.size(), 0); QCOMPARE(View->CaptureFocus(), AnchorKey); QVERIFY(View->CaptureSelection().contains(AnchorKey));
        QTRY_VERIFY(!Model->IsEverythingRunning()); QTRY_VERIFY(!Progress->isVisible()); QVERIFY(!Find->property("hc.quickFind.busy").toBool());
        QTRY_VERIFY(View->IsTwoLine()); QCOMPARE(View->ListMode(), QString("two-line")); QVERIFY(View->ListAppearance().nameBelow);
        const auto Profile = View->ColumnProfile(); QCOMPARE(Profile.mode_, hc::columns::LayoutMode::NameLast);
        const auto Location = std::find_if(Profile.columns_.begin(), Profile.columns_.end(), [](const auto& Column) { return Column.fieldId_ == "file.parentPath"; });
        QVERIFY(Location != Profile.columns_.end());
        for (int Column = 0; Column < Model->columnCount(); ++Column) if (View->ColumnWidths()->ColumnId(Column) == hc::columns::IdText(Location->id_)) {
            auto* PathColumn = static_cast<Qtitan::GridBandedTableColumn*>(View->BaseView()->getColumnByModelColumn(Column));
            QCOMPARE(PathColumn->rowIndex(), 0); QCOMPARE(PathColumn->visualIndex(), 1);
        }
        QCOMPARE(static_cast<Qtitan::GridBandedTableColumn*>(View->BaseView()->getColumnByModelColumn(0))->rowIndex(), 1);
        QVERIFY(Find->hasFocus()); const auto QueryText = Find->text(); const auto Caret = Find->cursorPosition();
        QTest::keyClick(Find, Qt::Key_Home); QCOMPARE(View->BaseView()->focusedRowIndex(), 0); QVERIFY(Find->hasFocus());
        QTest::keyClick(Find, Qt::Key_PageDown); const int PageDown = View->BaseView()->focusedRowIndex(); QVERIFY(PageDown > 0); QVERIFY(Find->hasFocus());
        QTest::keyClick(Find, Qt::Key_PageUp); QVERIFY(View->BaseView()->focusedRowIndex() < PageDown); QVERIFY(Find->hasFocus());
        QTest::keyClick(Find, Qt::Key_End); QCOMPARE(View->BaseView()->focusedRowIndex(), View->BaseView()->getRowCount() - 1); QVERIFY(Find->hasFocus());
        QCOMPARE(Find->text(), QueryText); QCOMPARE(Find->cursorPosition(), Caret);
        QSet<QString> Actual; int Same = 0;
        for (int Row = 0; Row < Model->rowCount(); ++Row) {
            const auto Index = Model->index(Row, 0); const auto Entry = Model->GetFileInfo(Index);
            QVERIFY(Entry && Entry->storageEntry_); QVERIFY(Entry->Name_ != "..");
            Actual.insert(QDir::fromNativeSeparators(Model->GetFileFullPath(Index)));
            if (Entry->Name_ == "hc-everything-same.txt") ++Same;
            if (hc::workspace::SelectionKey(*Entry->storageEntry_) == AnchorKey) QCOMPARE(Index.data(Qt::ForegroundRole), OriginalColor);
        }
        QCOMPARE(Actual.size(), 302); QCOMPARE(Same, 2); QVERIFY(!main_->findChild<QWidget*>("hc.quickFind.matches"));
        // A source in another folder must never resolve against the origin.
        const auto Wanted = QDir::toNativeSeparators(Dir.filePath("b/hc-everything-same.txt"));
        QModelIndex SourceIndex;
        for (int Row = 0; Row < Model->rowCount(); ++Row) if (Model->GetFileFullPath(Model->index(Row, 0)) == Wanted) SourceIndex = Model->index(Row, 0);
        QVERIFY(SourceIndex.isValid()); const auto CopyEntry = *Model->GetFileInfo(SourceIndex)->storageEntry_;
        const auto CopyKey = hc::workspace::SelectionKey(CopyEntry); View->RestoreSelection({CopyKey}, CopyKey);
        QVERIFY(Root.mkpath("copied"));
        auto* Bridge = TyStCommandMgr::GetInstance()->Bridge();
        auto Copy = Bridge->Dispatch("hc.file.copy", {{"destination", Dir.filePath("copied")}}, "everything-test", SourceIndex);
        QTRY_VERIFY(Copy.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready); const auto Copied = Copy.get();
        QVERIFY2(Copied.HasValue(), Copied.HasValue() ? "" : Copied.Failure().code_.c_str());
        auto& Scheduler = Bridge->FileJobs()->Scheduler();
        QTRY_COMPARE(Scheduler.Snapshot(Copied.Value().jobId_).Value().state_, hc::operations::JobState::Completed);
        QFile Output(Dir.filePath("copied/hc-everything-same.txt")); QVERIFY(Output.open(QIODevice::ReadOnly)); QCOMPARE(Output.readAll(), QByteArray("data-0"));
        QVERIFY(QFile::exists(Dir.filePath("a/hc-everything-same.txt"))); QVERIFY(Model->IsSearchResults());
        QVERIFY(!View->SetListMode("one-line")); QVERIFY(View->SetListMode("two-line")); QTest::qWait(60);
        QCOMPARE(Model->rowCount(), 302); QCOMPARE(View->BaseView()->getRowCount(), 302); QCOMPARE(AppendResets.size(), 0);
        if (!Capture.isEmpty()) QVERIFY(View->grab().save(QDir(Capture).filePath("everything-path-name.png")));
        QTest::keyClick(Find, Qt::Key_Escape); QTRY_VERIFY(!Model->IsSearchResults() && !Model->IsLoading());
        QCOMPARE(QDir::cleanPath(QDir::fromNativeSeparators(Panel->CurrentPath())), QDir::cleanPath(Dir.filePath("origin")));
        QTRY_COMPARE(View->CaptureFocus(), AnchorKey); QCOMPARE(Model->rowCount(), 2);
        QCOMPARE(View->ListMode(), QString("one-line")); QCOMPARE(View->CaptureWorkspaceView()["listPresentation"], OriginalPresentation);
        General["quickFind"] = QJsonObject{{"enabled", false}, {"scope", "panel"}, {"everythingDisplayIntervalMs", 1000}};
        QVERIFY(Settings.Put(hc::settings::Section::General, General)); QVERIFY(Settings.Commit());
        { QMainOpts Dialog(main_); Dialog.SetListView(View); Dialog.LoadSettings();
          auto* Interval = Dialog.findChild<QSpinBox*>("hc.navigation.quickFind.everythingDisplayIntervalMs");
          QVERIFY(Interval); QVERIFY(Interval->isEnabled()); QCOMPARE(Interval->value(), 1000); QCOMPARE(Interval->minimum(), 50); QCOMPARE(Interval->maximum(), 5000); }
        View->EnsureKeyboardFocusOnView();
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_F, Qt::ControlModifier | Qt::AltModifier); QTRY_VERIFY(Find->hasFocus());
        Find->setText("hc-everything"); QTRY_VERIFY(Model->IsSearchResults());
        QTRY_VERIFY(Progress->isVisible());
        QCOMPARE(Settings.CommittedObject()["general"].toObject()["quickFind"].toObject()["scope"].toString(), QString("panel"));
        QTest::keyClick(Find, Qt::Key_Escape); QTRY_VERIFY(!Model->IsSearchResults() && !Model->IsLoading()); QTest::qWait(250);
        QCOMPARE(Model->rowCount(), 2);
        QVERIFY(!Progress->isVisible());
    }

    void todoBugSearchEscapeFocus()
    {
        auto& Settings = hc::settings::AppSettings(); const auto Before = Settings.Object(); const auto Workspace = main_->CaptureWorkspace();
        auto* Find = main_->findChild<QLineEdit*>("hc.mainFind"); QVERIFY(Find);
        const auto Restore = qScopeGuard([&] { Find->clear(); Settings.Replace(Before); Settings.Commit(); main_->RestoreWorkspace(Workspace); });
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        for (const auto* Name : {"alpha.txt", "beta.txt"}) { QFile File(Directory.filePath(Name)); QVERIFY(File.open(QIODevice::WriteOnly)); }
        main_->raise(); main_->activateWindow(); QVERIFY(QTest::qWaitForWindowActive(main_));
        for (auto* Panel : panels_) {
            Panel->ChangeCurrentPath(Directory.path(), {}); QTRY_VERIFY(!model(Panel)->IsLoading());
            auto* View = static_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex()));
            for (const auto* Mode : {"one-line", "two-line", "thumbnails"}) {
                QVERIFY(View->SetListMode(Mode)); QTest::qWait(50);
                for (const bool Enabled : {true, false}) {
                    auto General = Settings.Read(hc::settings::Section::General).toObject();
                    General["quickFind"] = QJsonObject{{"enabled", Enabled}, {"activation", "ctrl-alt"}};
                    QVERIFY(Settings.Put(hc::settings::Section::General, General));
                    View->EnsureKeyboardFocusOnView(); emit Panel->sig_NotifyPanelActivated();
                    QVERIFY(QApplication::focusWidget() && View->isAncestorOf(QApplication::focusWidget()));
                    QTest::keyClick(QApplication::focusWidget(), Qt::Key_F, Qt::ControlModifier); QTRY_VERIFY(Find->hasFocus());
                    Find->setText("name:alpha"); QTRY_COMPARE(View->BaseView()->getRowCount(), 2);
                    QTest::keyClick(Find, Qt::Key_Escape);
                    QTRY_VERIFY(QApplication::focusWidget() && View->isAncestorOf(QApplication::focusWidget()));
                    QVERIFY(Find->text().isEmpty()); QVERIFY(Panel->Search().isEmpty()); QVERIFY(Panel->SearchMatch().isEmpty());
                    QTRY_COMPARE(View->BaseView()->getRowCount(), 3);
                    Find->clear(); QTest::mouseClick(Find, Qt::LeftButton); QTRY_VERIFY(Find->hasFocus());
                    Find->setText(QStringLiteral("ㄹ어ㅏㅣㄹ너ㅏㅣ")); QTRY_COMPARE(View->BaseView()->getRowCount(), 1);
                    QTest::keyClick(Find, Qt::Key_Escape);
                    QTRY_VERIFY(QApplication::focusWidget() && View->isAncestorOf(QApplication::focusWidget()));
                    QVERIFY(Find->text().isEmpty()); QVERIFY(Panel->Search().isEmpty());
                    QTRY_COMPARE(View->BaseView()->getRowCount(), 3);
                }
            }
        }
    }

    void todoBugQuickFindCursor()
    {
        auto& Settings = hc::settings::AppSettings(); const auto Before = Settings.Object(); const auto Workspace = main_->CaptureWorkspace();
        auto* Find = main_->findChild<QLineEdit*>("hc.mainFind"); QVERIFY(Find);
        const auto Restore = qScopeGuard([&] { QTest::keyClick(Find, Qt::Key_Escape); Settings.Replace(Before); Settings.Commit(); main_->RestoreWorkspace(Workspace); });
        auto General = Settings.Read(hc::settings::Section::General).toObject();
        // Missing result key must migrate to cursor movement.
        General["quickFind"] = QJsonObject{{"enabled", true}, {"activation", "typing"}, {"scope", "panel"}, {"match", "starts"}};
        QVERIFY(Settings.Put(hc::settings::Section::General, General));
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        for (const auto& Name : {QString("AGENTS.md"), QString("AGENTS-extra.md"), QString("alpha.txt"), QString("my-AG.txt"), QString("notes.txt"), QString("한글.txt")}) {
            QFile File(Directory.filePath(Name)); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("cursor find");
        }
        auto* Panel = panels_.front(); Panel->ChangeCurrentPath(Directory.path(), {}); QTRY_VERIFY(!model(Panel)->IsLoading());
        auto* Model = static_cast<CFSModel*>(model(Panel)); auto* View = static_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex()));
        const auto FocusName = [&] { auto Index = View->BaseView()->focusedRow().modelIndex(0); while (const auto* Proxy = qobject_cast<const QAbstractProxyModel*>(Index.model())) Index = Proxy->mapToSource(Index); return Index.isValid() ? Model->GetName(Index) : QString{}; };
        const auto Key = [&](const QString& Name) { for (int Row = 0; Row < Model->rowCount(); ++Row) { const auto Item = Model->GetFileInfo(Model->index(Row, 0)); if (Item && Item->storageEntry_ && Item->Name_ == Name) return hc::workspace::SelectionKey(*Item->storageEntry_); } return QString{}; };
        const auto Mark = Key("notes.txt"); QVERIFY(!Mark.isEmpty());
        for (const auto* Mode : {"one-line", "two-line", "thumbnails"}) {
            TyStCommandMgr::GetInstance()->Bridge()->ExecuteCommand("hc.search.focus"); Find->clear();
            QVERIFY(View->SetListMode(Mode)); QTest::qWait(100); View->RestoreSelection({Mark}, Mark);
            const int Count = View->BaseView()->getRowCount(); QCOMPARE(Count, 7);
            QStringList Matches;
            for (int Row = 0; Row < Count; ++Row) {
                auto Index = View->BaseView()->getRow(Row).modelIndex(0); while (const auto* Proxy = qobject_cast<const QAbstractProxyModel*>(Index.model())) Index = Proxy->mapToSource(Index);
                if (Index.isValid() && Model->GetName(Index).startsWith("AG", Qt::CaseInsensitive)) Matches.append(Model->GetName(Index));
            }
            QCOMPARE(Matches.size(), 2);
            View->EnsureKeyboardFocusOnView(); QTest::qWait(30); auto* Target = QApplication::focusWidget(); QVERIFY(Target && Target != Find);
            QTest::keyClick(Target, Qt::Key_A); QTRY_VERIFY(Find->hasFocus()); QTest::keyClicks(Find, "G");
            QCOMPARE(Find->text(), QString("aG")); QCOMPARE(FocusName(), Matches[0]); QCOMPARE(View->BaseView()->getRowCount(), Count);
            QVERIFY(View->property("hc.quickFind.highlight").toBool()); QCOMPARE(View->property("hc.quickFind.text").toString(), Find->text());
            QVERIFY(SaveR9Dialog(*main_, "quick-find-highlight-" + QString::fromLatin1(Mode)));
            QVERIFY(Panel->Search().isEmpty()); QCOMPARE(View->CaptureSelection(), QStringList{Mark});
            QTest::keyClick(Find, Qt::Key_Down); QCOMPARE(FocusName(), Matches[1]); QVERIFY(Find->hasFocus());
            QTest::keyClick(Find, Qt::Key_Down); QCOMPARE(FocusName(), Matches[0]);
            QTest::keyClick(Find, Qt::Key_Up); QCOMPARE(FocusName(), Matches[1]);
            const auto Last = FocusName(); Find->setText("no-such-file"); QCOMPARE(FocusName(), Last); QCOMPARE(View->BaseView()->getRowCount(), Count);
            Find->setText("AG"); QCOMPARE(FocusName(), Matches[0]);
            const auto Query = Find->text(); const auto Caret = Find->cursorPosition();
            QTest::keyClick(Find, Qt::Key_S, Qt::ControlModifier); QCOMPARE(View->BaseView()->getRowCount(), 3);
            QCOMPARE(Panel->Search(), Query); QCOMPARE(Find->text(), Query); QCOMPARE(Find->cursorPosition(), Caret); QVERIFY(Find->hasFocus());
            QTest::keyClick(Find, Qt::Key_S, Qt::ControlModifier); QCOMPARE(View->BaseView()->getRowCount(), Count);
            QCOMPARE(Find->text(), Query); QCOMPARE(FocusName(), Matches[0]); QVERIFY(Find->hasFocus());
            QTest::keyClick(Find, Qt::Key_Return); QVERIFY(!Find->hasFocus());
            Target = QApplication::focusWidget(); QVERIFY(Target);
            QTest::keyClick(Target, Qt::Key_Down); QCOMPARE(FocusName(), Matches[1]);
            QTest::keyClick(Target, Qt::Key_Escape); QCOMPARE(Find->text(), QString{});
        }
        TyStCommandMgr::GetInstance()->Bridge()->ExecuteCommand("hc.search.focus"); Find->clear();
        QTest::keyClick(Find, Qt::Key_S, Qt::ControlModifier); // Standard filter -> cursor mode.
        QInputMethodEvent Preedit(QStringLiteral("한"), {}); QApplication::sendEvent(Find, &Preedit);
        QTest::keyClick(Find, Qt::Key_S, Qt::ControlModifier); QCOMPARE(Find->text(), QString{});
        QInputMethodEvent Commit; Commit.setCommitString(QStringLiteral("한")); QApplication::sendEvent(Find, &Commit);
        QTRY_COMPARE(Find->text(), QStringLiteral("한")); QTRY_COMPARE(View->BaseView()->getRowCount(), 2);
        QTest::keyClick(Find, Qt::Key_S, Qt::ControlModifier); QTRY_COMPARE(View->BaseView()->getRowCount(), 7); QCOMPARE(FocusName(), QStringLiteral("한글.txt"));
        QVERIFY(SaveR9Dialog(*main_, "quick-find-cursor"));
    }

    void todoBugQuickFindHighlight()
    {
        using hc::quickfind::HighlightRanges;
        QCOMPARE(HighlightRanges("AGENTS.md", "ag"), (QList<QPair<int,int>>{{0, 2}}));
        QCOMPARE(HighlightRanges("x-ag-ag.txt", "AG"), (QList<QPair<int,int>>{{2, 2}, {5, 2}}));
        QCOMPARE(HighlightRanges("x-ag.txt", "ag", "starts").size(), 0);
        QCOMPARE(HighlightRanges("x-ag.txt", "TXT", "ends"), (QList<QPair<int,int>>{{5, 3}}));
        const auto Nfd = QStringLiteral("한글").normalized(QString::NormalizationForm_D);
        QCOMPARE(HighlightRanges(Nfd + ".txt", "한"), (QList<QPair<int,int>>{{0, 3}}));
        QCOMPARE(HighlightRanges(QStringLiteral("Straße.txt"), QStringLiteral("ß")), (QList<QPair<int,int>>{{4, 1}}));
        QCOMPARE(HighlightRanges(QString::fromUcs4(U"😀한글.txt"), "한"), (QList<QPair<int,int>>{{2, 1}}));
        QVERIFY(HighlightRanges("AGENTS.md", {}).isEmpty());
        QTreeWidget Tree; Tree.setColumnCount(2); auto* Item = new QTreeWidgetItem(&Tree, {"AGENTS-AG.md", "C:/test"});
        hc::quickfind::ResultHighlightDelegate Delegate(&Tree);
        Tree.setProperty("hc.quickFind.text", "ag");
        const auto YellowPixels = [&](bool Enabled) {
            Tree.setProperty("hc.quickFind.highlight", Enabled);
            QImage Image(360, 48, QImage::Format_ARGB32_Premultiplied); Image.fill(Qt::white);
            QPainter Painter(&Image); QStyleOptionViewItem Option; Option.rect = Image.rect(); Option.font = main_->font();
            Option.state = QStyle::State_Enabled | QStyle::State_Selected; Option.widget = &Tree;
            Delegate.paint(&Painter, Option, Tree.indexFromItem(Item)); Painter.end();
            int Count = 0; for (int Y = 0; Y < Image.height(); ++Y) for (int X = 0; X < Image.width(); ++X)
                if (Image.pixelColor(X,Y) == QColor("#ffd86b")) ++Count;
            return Count;
        };
        QVERIFY(YellowPixels(true) > 20); QCOMPARE(YellowPixels(false), 0);
    }

    void todoBugQuickFindEverything()
    {
        auto& Settings = hc::settings::AppSettings(); const auto Before = Settings.Object(); const auto Workspace = main_->CaptureWorkspace();
        auto* Find = main_->findChild<QLineEdit*>("hc.mainFind"); QVERIFY(Find);
        const auto Restore = qScopeGuard([&] { QTest::keyClick(Find, Qt::Key_Escape); Settings.Replace(Before); Settings.Commit(); main_->RestoreWorkspace(Workspace); });
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        const auto Stem = "hcqf" + QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-');
        const auto Path = Directory.filePath(Stem + ".txt"); { QFile File(Path); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("indexed quick find"); }
        for (int I = 0; I < 128; ++I) { QFile File(Directory.filePath(Stem + QString("_%1.txt").arg(I, 3, 10, QChar('0')))); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("indexed extra"); }
        const auto Query = [&](const QString& Text, const QString& Match) {
            QProcess Process; Process.start(QCoreApplication::applicationDirPath() + "/hc-search-index.exe", {"--quick-find", Text, Match});
            if (!Process.waitForFinished(6000) || Process.exitCode() != 0) return false;
            const auto Paths = QJsonDocument::fromJson(Process.readAllStandardOutput())["paths"].toArray();
            for (const auto& Value : Paths) if (QDir::cleanPath(QDir::fromNativeSeparators(Value.toString())).compare(Path, Qt::CaseInsensitive) == 0) return true;
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(Query(Stem + ".txt", "starts"), 30000);
        QVERIFY(Query(Stem.mid(4), "contains")); QVERIFY(Query(Stem.mid(4) + ".txt", "ends"));
        auto General = Settings.Read(hc::settings::Section::General).toObject();
        General["quickFind"] = QJsonObject{{"enabled", true}, {"activation", "typing"}, {"scope", "everything"}, {"match", "starts"}};
        QVERIFY(Settings.Put(hc::settings::Section::General, General));
        auto* Panel = panels_.front(); Panel->SetSearch({});
        auto* View = static_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex())); View->EnsureKeyboardFocusOnView(); QTest::qWait(30);
        auto* Target = QApplication::focusWidget(); QVERIFY(Target && Target != Find);
        QTest::keyClick(Target, Qt::Key_H); QTRY_VERIFY(Find->hasFocus()); QTest::keyClicks(Find, Stem.mid(1));
        auto* Results = static_cast<CFSModel*>(model(Panel));
        QTRY_VERIFY_WITH_TIMEOUT(Results->IsSearchResults() && Results->rowCount() == 129, 15000);
        QSet<QString> Found;
        for (int Row = 0; Row < Results->rowCount(); ++Row) Found.insert(QDir::fromNativeSeparators(Results->GetFileFullPath(Results->index(Row, 0))));
        QCOMPARE(Found.size(), 129); QVERIFY(Found.contains(Path));
        QVERIFY(Panel->Search().isEmpty());
        QVERIFY(SaveR9Dialog(*main_, "quick-find-everything"));
        QTest::keyClick(Find, Qt::Key_Return);
        QTRY_VERIFY(QApplication::focusWidget() && View->isAncestorOf(QApplication::focusWidget()));
        Panel->OpenSearchLocation();
        QTRY_VERIFY(!Results->IsSearchResults() && !Results->IsLoading() && QDir::fromNativeSeparators(Panel->CurrentPath()) == Directory.path());
        QVERIFY(!main_->findChild<QWidget*>("hc.quickFind.results"));
    }

    void todoBugQuickFindNativeIme()
    {
        if (!qEnvironmentVariableIsSet("HC_TEST_QUICK_FIND_NATIVE_IME")) QSKIP("Native Korean IME test requires the interactive desktop opt-in.");
        auto& Settings = hc::settings::AppSettings(); const auto Before = Settings.Object(); const auto Workspace = main_->CaptureWorkspace();
        const auto PreviousLayout = GetKeyboardLayout(0); const auto PreviousForeground = GetForegroundWindow();
        const auto Restore = qScopeGuard([&] { ActivateKeyboardLayout(PreviousLayout, 0); Settings.Replace(Before); Settings.Commit(); main_->RestoreWorkspace(Workspace); if (IsWindow(PreviousForeground)) SetForegroundWindow(PreviousForeground); });
        auto General = Settings.Read(hc::settings::Section::General).toObject();
        General["quickFind"] = QJsonObject{{"enabled", true}, {"activation", "typing"}, {"scope", "panel"}, {"match", "starts"}};
        QVERIFY(Settings.Put(hc::settings::Section::General, General));
        auto* Find = main_->findChild<QLineEdit*>("hc.mainFind"); QVERIFY(Find);
        TyStCommandMgr::GetInstance()->Bridge()->ExecuteCommand("hc.search.focus"); Find->clear();
        auto* Panel = panels_.front(); auto* View = static_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex()));
        main_->raise(); main_->activateWindow();
        const auto Window = reinterpret_cast<HWND>(main_->winId());
        const auto ForegroundThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
        const bool Attached = ForegroundThread && ForegroundThread != GetCurrentThreadId() && AttachThreadInput(GetCurrentThreadId(), ForegroundThread, TRUE);
        SetForegroundWindow(Window); if (Attached) AttachThreadInput(GetCurrentThreadId(), ForegroundThread, FALSE);
        QTRY_VERIFY(GetForegroundWindow() == Window);
        const auto Korean = LoadKeyboardLayoutW(L"00000412", KLF_NOTELLSHELL); QVERIFY(Korean);
        QVERIFY(ActivateKeyboardLayout(Korean, 0)); QTest::qWait(100);
        View->EnsureKeyboardFocusOnView(); QTest::qWait(100);
        auto* Target = QApplication::focusWidget(); QVERIFY(Target && Target != Find);
        const auto Imm = LoadLibraryW(L"imm32.dll"); QVERIFY(Imm); const auto ReleaseImm = qScopeGuard([&] { FreeLibrary(Imm); });
        const auto GetContext = reinterpret_cast<HIMC(WINAPI*)(HWND)>(GetProcAddress(Imm, "ImmGetContext"));
        const auto ReleaseContext = reinterpret_cast<BOOL(WINAPI*)(HWND,HIMC)>(GetProcAddress(Imm, "ImmReleaseContext"));
        const auto SetOpen = reinterpret_cast<BOOL(WINAPI*)(HIMC,BOOL)>(GetProcAddress(Imm, "ImmSetOpenStatus"));
        const auto SetConversion = reinterpret_cast<BOOL(WINAPI*)(HIMC,DWORD,DWORD)>(GetProcAddress(Imm, "ImmSetConversionStatus"));
        const auto GetOpen = reinterpret_cast<BOOL(WINAPI*)(HIMC)>(GetProcAddress(Imm, "ImmGetOpenStatus"));
        const auto GetConversion = reinterpret_cast<BOOL(WINAPI*)(HIMC,LPDWORD,LPDWORD)>(GetProcAddress(Imm, "ImmGetConversionStatus"));
        QVERIFY(GetContext && ReleaseContext && SetOpen && SetConversion && GetOpen && GetConversion);
        const auto Context = GetContext(Window); QVERIFY(Context);
        const auto PreviousOpen = GetOpen(Context); DWORD PreviousConversion = 0, PreviousSentence = 0;
        QVERIFY(GetConversion(Context, &PreviousConversion, &PreviousSentence));
        const auto RestoreIme = qScopeGuard([&] { const auto Current = GetContext(Window); if (Current) { SetOpen(Current, PreviousOpen); SetConversion(Current, PreviousConversion, PreviousSentence); ReleaseContext(Window, Current); } });
        QVERIFY(SetOpen(Context, TRUE)); QVERIFY(SetConversion(Context, IME_CMODE_NATIVE, 0)); ReleaseContext(Window, Context);
        const auto Key = [&](WORD Value) {
            INPUT Events[2]{}; Events[0].type = Events[1].type = INPUT_KEYBOARD;
            Events[0].ki.wVk = Events[1].ki.wVk = Value; Events[1].ki.dwFlags = KEYEVENTF_KEYUP;
            return GetForegroundWindow() == Window && SendInput(2, Events, sizeof(INPUT)) == 2;
        };
        // Korean two-set keyboard: g k s r m f -> 한글. These are native key events,
        // not synthetic commit strings, so the Windows composition/focus path runs.
        int ComposingKeys = 0;
        for (const WORD Value : {'G', 'K', 'S', 'R', 'M', 'F'}) {
            QVERIFY(Key(Value)); QTest::qWait(80);
            if (++ComposingKeys == 3) {
                INPUT Toggle[4]{}; const WORD Keys[] = {VK_CONTROL, 'S', 'S', VK_CONTROL};
                for (int I = 0; I < 4; ++I) { Toggle[I].type = INPUT_KEYBOARD; Toggle[I].ki.wVk = Keys[I]; if (I >= 2) Toggle[I].ki.dwFlags = KEYEVENTF_KEYUP; }
                QVERIFY(GetForegroundWindow() == Window); QCOMPARE(SendInput(4, Toggle, sizeof(INPUT)), UINT(4)); QTest::qWait(80);
            }
        }
        QVERIFY(Key(VK_SPACE)); QTest::qWait(150);
        QVERIFY(Find->hasFocus()); QCOMPARE(Find->text().normalized(QString::NormalizationForm_C), QStringLiteral("한글 "));
        qInfo() << "Native Korean IME typing verified:" << Find->text();
        QTest::keyClick(Find, Qt::Key_Escape);
        auto Quick = General["quickFind"].toObject(); Quick["activation"] = "ctrl-alt"; General["quickFind"] = Quick;
        QVERIFY(Settings.Put(hc::settings::Section::General, General)); View->EnsureKeyboardFocusOnView(); QTest::qWait(50);
        INPUT Chord[6]{}; const WORD ChordKeys[] = {VK_CONTROL, VK_MENU, 'A', 'A', VK_MENU, VK_CONTROL};
        for (int I = 0; I < 6; ++I) { Chord[I].type = INPUT_KEYBOARD; Chord[I].ki.wVk = ChordKeys[I]; if (I >= 3) Chord[I].ki.dwFlags = KEYEVENTF_KEYUP; }
        QVERIFY(GetForegroundWindow() == Window); QCOMPARE(SendInput(6, Chord, sizeof(INPUT)), UINT(6));
        QTRY_VERIFY(Find->hasFocus()); const auto ChordText = Find->text(); QCOMPARE(ChordText.toLower(), QString("a"));
        for (const WORD Value : {'G', 'K', 'S', 'R', 'M', 'F'}) { QVERIFY(Key(Value)); QTest::qWait(80); }
        QVERIFY(Key(VK_SPACE)); QTest::qWait(150);
        QCOMPARE(Find->text().normalized(QString::NormalizationForm_C), ChordText + QStringLiteral("한글 "));
        qInfo() << "Native Ctrl+Alt activation then Korean IME typing verified:" << Find->text();
        QTest::keyClick(Find, Qt::Key_Escape);
    }

    void todoBugHeaderButtonHeights()
    {
        auto* Jobs = main_->findChild<QPushButton*>("hc.jobs.indicator"); QVERIFY(Jobs);
        auto* Settings = main_->findChild<QToolButton*>("hc.menuRecovery"); QVERIFY(Settings);
        auto* Update = main_->findChild<QToolButton*>("hc.update.menuButton"); QVERIFY(Update);
        const auto Original = main_->font();
        const auto Restore = qScopeGuard([&] { main_->setFont(Original); });
        for (const int Points : {10, 14, 20}) {
            auto Font = Original; Font.setPointSize(Points); main_->setFont(Font); QTest::qWait(100);
            QCOMPARE(Jobs->geometry().top(), Settings->geometry().top());
            QCOMPARE(Settings->geometry().top(), Update->geometry().top());
            QCOMPARE(Jobs->height(), Settings->height());
            QCOMPARE(Settings->height(), Update->height());
            for (auto* Button : {static_cast<QWidget*>(Jobs), static_cast<QWidget*>(Settings), static_cast<QWidget*>(Update)}) {
                QVERIFY(Button->isVisible());
                QVERIFY(Button->height() >= Button->fontMetrics().height() + 8);
            }
        }
    }

    void todoBugHelpMenuPlacement()
    {
        auto* Bar = main_->findChild<QMenuBar*>("menubar"); QVERIFY(Bar);
        QVERIFY(!Bar->cornerWidget(Qt::TopRightCorner));
        auto* Corner = main_->findChild<QWidget*>("hc.headerActions"); QVERIFY(Corner);
        QCOMPARE(Corner->parentWidget(), Bar->parentWidget());
        auto* Help = main_->findChild<QToolButton*>("hc.update.menuButton"); QVERIFY(Help);
        QCOMPARE(Help->parentWidget(), Corner);
        QVERIFY(Corner->layout()->indexOf(Help) >= 0);
        const auto OriginalSize = main_->size();
        const auto OriginalFont = Help->font();
        const auto Restore = qScopeGuard([&] { Help->setFont(OriginalFont); main_->resize(OriginalSize); });
        for (const int Width : {1360, 800}) {
            main_->resize(Width, OriginalSize.height());
            auto Font = OriginalFont; Font.setPointSize(14); Help->setFont(Font);
            QTest::qWait(100);
            QVERIFY(Help->isVisible());
            const QRect HelpRect(Help->mapTo(Bar->parentWidget(), QPoint()), Help->size());
            qInfo() << "Header geometry:" << Bar->parentWidget()->rect() << Bar->geometry() << Corner->geometry() << HelpRect;
            QVERIFY(SaveR9Dialog(*main_, "header-layout"));
            qInfo() << "Header after theme:" << Bar->parentWidget()->rect() << Bar->geometry() << Corner->geometry();
            QVERIFY(Bar->parentWidget()->rect().contains(HelpRect));
            QTRY_VERIFY(!Bar->geometry().intersects(Corner->geometry()));
            for (auto* Action : Bar->actions()) {
                const auto Rect = Bar->actionGeometry(Action);
                if (!Action->isVisible() || Rect.isEmpty() || !Bar->rect().contains(Rect)) continue;
                QCOMPARE(Bar->actionAt(Rect.center()), Action);
                QTest::mouseClick(Bar, Qt::LeftButton, Qt::NoModifier, Rect.center());
                QTRY_VERIFY(Action->menu()->isVisible());
                Action->menu()->hide();
            }
            auto* HelpMenu = Help->menu(); QVERIFY(HelpMenu);
            QVERIFY(HelpMenu->findChild<QAction*>("hc.update.check"));
            QSignalSpy Opened(HelpMenu, &QMenu::aboutToShow);
            QTimer::singleShot(100, HelpMenu, [HelpMenu] { HelpMenu->hide(); });
            QTest::mouseClick(Help, Qt::LeftButton);
            QCOMPARE(Opened.count(), 1);
        }
        QVERIFY(SaveR9Dialog(*main_, "help-menu-placement"));
    }

    void menuBarSettingsUi()
    {
        auto& Settings = hc::settings::AppSettings(); const auto OriginalSettings = Settings.Object();
        const auto Path = QDir(hc::settings::Paths().directory_).filePath("menu-bar.json");
        QFile OriginalFile(Path); QVERIFY(OriginalFile.open(QIODevice::ReadOnly)); const auto Original = OriginalFile.readAll(); OriginalFile.close();
        const auto Write = [&](const QByteArray& Data) { QSaveFile File(Path); return File.open(QIODevice::WriteOnly) && File.write(Data) == Data.size() && File.commit(); };
        const auto Restore = qScopeGuard([&] { Settings.DiscardEdit(); Settings.Replace(OriginalSettings); Settings.Commit(); Write(Original); TyStCommandMgr::GetInstance()->Bridge()->ReloadMenu(); });
        const auto Change = [](hc::ui::MenuBarSettingsPage* Page, const QString& Label) {
            auto Doc = Page->Document(); auto Menus = Doc["menus"].toArray(); auto First = Menus[0].toObject(); First["label"] = Label; Menus[0] = First; Doc["menus"] = Menus; return Page->Import(QJsonDocument(Doc).toJson());
        };
        { QMainOpts Dialog(main_); Dialog.show(); QTest::qWait(80); Dialog.LoadSettings();
          auto* Page = static_cast<hc::ui::MenuBarSettingsPage*>(Dialog.findChild<QWidget*>("hc.settings.menuBar")); QVERIFY(Page);
          QVERIFY(Change(Page, "취소 메뉴(&Z)")); QVERIFY(Page->Dirty()); Dialog.reject(); }
        { QFile File(Path); QVERIFY(File.open(QIODevice::ReadOnly)); QCOMPARE(File.readAll(), Original); }
        { hc::ui::MenuBarSettingsPage Page; QVERIFY(Change(&Page, "외부 충돌(&Z)"));
          const auto External = Original + "\r\n"; QVERIFY(Write(External)); QVERIFY(!Page.Prepare());
          QFile File(Path); QVERIFY(File.open(QIODevice::ReadOnly)); QCOMPARE(File.readAll(), External); File.close(); QVERIFY(Write(Original)); }
        { hc::ui::MenuBarSettingsPage Page; QVERIFY(Change(&Page, "잠금 메뉴(&Z)")); QLockFile Lock(Path + ".lock"); Lock.setStaleLockTime(0); QVERIFY(Lock.tryLock(0)); QVERIFY(!Page.Prepare()); }
        { hc::ui::MenuBarSettingsPage Page; const auto Before = Page.Document(); QVERIFY(!Page.Import("{invalid")); QCOMPARE(Page.Document(), Before);
          QVERIFY(!Page.Import(R"({"schemaVersion":2,"menus":[]})")); QCOMPARE(Page.Document(), Before); }
        { QMainOpts Dialog(main_); Dialog.show(); QTest::qWait(80); Dialog.LoadSettings();
          auto* Page = static_cast<hc::ui::MenuBarSettingsPage*>(Dialog.findChild<QWidget*>("hc.settings.menuBar")); QVERIFY(Change(Page, "저장 실패(&Z)"));
          QLockFile Lock(Settings.FileName() + ".lock"); Lock.setStaleLockTime(0); QVERIFY(Lock.tryLock(0));
          QTimer::singleShot(0, [] { for (auto* W : QApplication::topLevelWidgets()) if (auto* Message = qobject_cast<QMessageBox*>(W)) Message->reject(); });
          QVERIFY(!Dialog.SaveSettings()); QFile File(Path); QVERIFY(File.open(QIODevice::ReadOnly)); QCOMPARE(File.readAll(), Original); Dialog.reject(); }
        { QMainOpts Dialog(main_); Dialog.show(); QTest::qWait(80); Dialog.LoadSettings();
          auto* Page = static_cast<hc::ui::MenuBarSettingsPage*>(Dialog.findChild<QWidget*>("hc.settings.menuBar")); QVERIFY(Change(Page, "사용자 파일(&Z)"));
          QVERIFY(Dialog.SaveSettings()); QVERIFY(!Page->Dirty());
          auto* Menu = main_->findChild<QMenuBar*>("menubar"); QCOMPARE(Menu->actions().front()->text(), QString("사용자 파일(&Z)"));
          auto* Sidebar = Dialog.findChild<QListWidget*>("lstOptCate"); Sidebar->setCurrentRow(Sidebar->count() - 1);
          const auto Artifacts = qEnvironmentVariable("HC_UI_ARTIFACT_DIR"); if (!Artifacts.isEmpty()) { QTest::qWait(50); QVERIFY(Dialog.grab().save(QDir(Artifacts).filePath("r5-menu-settings.png"))); }
          Dialog.reject(); }
        { hc::ui::MenuBarSettingsPage Page; QCOMPARE(Page.Document()["menus"].toArray()[0].toObject()["label"].toString(), QString("사용자 파일(&Z)")); }
    }

    void thumbnailSettingsUi()
    {
        auto& Settings = hc::settings::AppSettings(); const auto Original = Settings.Object(); const auto Workspace = main_->CaptureWorkspace();
        const auto Restore = qScopeGuard([&] { Settings.Replace(Original); Settings.Commit(); main_->RestoreWorkspace(Workspace); main_->SaveWorkspace(); });
        auto* View = static_cast<CGridView*>(panels_.front()->GetFocusView(panels_.front()->CurrentTabIndex())); QVERIFY(View->SetListMode("thumbnails"));
        const auto Before = Settings.Read(hc::settings::Section::General);
        {
            hc::ui::ThumbnailSettingsPage Page(main_); Page.Load({});
            for (const auto* Name : {"hc.settings.thumbSize", "hc.settings.thumbLines", "hc.settings.thumbInfo", "hc.settings.thumbFit"}) {
                auto* Choice = Page.findChild<QComboBox*>(Name); QVERIFY(Choice);
                for (int I = 0; I < Choice->count(); ++I) { auto* Button = Page.findChild<QPushButton*>(QString(Name) + ".choice" + QString::number(I)); QVERIFY(Button); Button->click(); QCOMPARE(Choice->currentIndex(), I); QVERIFY(Button->isChecked()); }
            }
            const auto Draft = Page.Save(); hc::ui::ThumbnailSettingsPage Reload(main_); Reload.Load(Draft); QCOMPARE(Reload.Save(), Draft);
            Page.findChild<QComboBox*>("hc.settings.thumbSize")->setCurrentIndex(0);
            auto* Preview = Page.findChild<fm::filelist::ThumbnailView*>("hc.settings.thumbPreview"); QVERIFY(Preview); QCOMPARE(Preview->model()->rowCount(), 9);
            QVERIFY(Preview->appearance().nameLines > 3); QCOMPARE(Page.Save()["nameLines"].toInt(), 0);
            Page.resize(1000, 700); Page.show(); QTest::qWait(40); Page.findChild<QComboBox*>("hc.settings.thumbSize")->setCurrentIndex(3); QTest::qWait(40);
            auto* List = Preview->listView(); const auto Last = Preview->model()->index(8, List->modelColumn()); List->scrollTo(Last, QAbstractItemView::PositionAtBottom); QCoreApplication::processEvents();
            QVERIFY(List->viewport()->rect().intersects(List->visualRect(Last))); QVERIFY(List->verticalScrollBar()->maximum() > 0);
            const auto BeforePreview = Page.Save(); Page.findChild<QCheckBox*>("hc.thumbs.previewInverseCursor")->setChecked(true); Page.findChild<QCheckBox*>("hc.thumbs.previewInverseSelection")->setChecked(true);
            QVERIFY(Preview->appearance().invertCursor); QVERIFY(Preview->appearance().invertSelection); QCOMPARE(Page.Save(), BeforePreview);
            auto* Method = Page.findChild<QComboBox*>("hc.settings.thumbMethod"); Method->setCurrentIndex(1);
            auto* Targets = Page.findChild<QPushButton*>("hc.settings.thumbTargets"); QVERIFY(Targets->menu()->actions()[0]->isEnabled()); QVERIFY(!Targets->menu()->actions()[1]->isEnabled());
            QCOMPARE(Page.Save()["targets"], BeforePreview["targets"]); Method->setCurrentIndex(2); QVERIFY(Targets->menu()->actions()[1]->isEnabled()); QCOMPARE(Page.Save(), BeforePreview);
            Preview->sizeChanged(160); QCOMPARE(Page.Save()["size"].toInt(), 160); QVERIFY(Page.findChild<QPushButton*>("hc.settings.thumbSize.choice2")->isChecked());
            auto DraftWithMetadata = Page.Save(); DraftWithMetadata["extensionMetadata"] = "preserve"; Page.Load(DraftWithMetadata);
            auto* Clear = Page.findChild<QPushButton*>("hc.settings.thumbClear"); Clear->click(); QVERIFY(Page.ClearRequested()); Clear->click(); QVERIFY(!Page.ClearRequested()); QCOMPARE(Page.Save(), DraftWithMetadata);
            Clear->click(); Page.findChild<QPushButton*>("hc.settings.thumbReset")->click(); QVERIFY(!Page.ClearRequested());
            const auto DefaultDraft = Page.Save(); QCOMPARE(DefaultDraft["extensionMetadata"].toString(), QString("preserve")); QCOMPARE(DefaultDraft["size"].toInt(), 96); QCOMPARE(DefaultDraft["targets"].toInt(), 15); QCOMPARE(DefaultDraft["autoPercent"].toInt(), 70);
            QVERIFY(!Page.findChild<QSpinBox*>("hc.settings.thumbPercent")->isEnabled()); Page.findChild<QCheckBox*>("hc.settings.thumbAuto")->setChecked(true); QVERIFY(Page.findChild<QSpinBox*>("hc.settings.thumbPercent")->isEnabled());
            for (int Size = 0; Size < 4; ++Size) for (int Lines = 0; Lines < 3; ++Lines) for (int Info = 0; Info < 4; ++Info) for (int Fill = 0; Fill < 2; ++Fill) {
                Page.findChild<QComboBox*>("hc.settings.thumbSize")->setCurrentIndex(Size); Page.findChild<QComboBox*>("hc.settings.thumbLines")->setCurrentIndex(Lines);
                Page.findChild<QComboBox*>("hc.settings.thumbInfo")->setCurrentIndex(Info); Page.findChild<QComboBox*>("hc.settings.thumbFit")->setCurrentIndex(Fill);
                QCOMPARE(Preview->appearance().size, fm::filelist::kThumbnailSizes[Size]); QCOMPARE(int(Preview->appearance().info), Info); QCOMPARE(Preview->appearance().fill, Fill != 0);
                if (Lines < 2) QCOMPARE(Preview->appearance().nameLines, Lines + 1); else QVERIFY(Preview->appearance().nameLines >= 2);
                Reload.Load(Page.Save()); QCOMPARE(Reload.Save(), Page.Save());
            }
        }
        { QMainOpts Dialog(main_); Dialog.show(); QTest::qWait(80); Dialog.LoadSettings();
          Dialog.findChild<QComboBox*>("hc.settings.thumbSize")->setCurrentIndex(3); Dialog.findChild<QPushButton*>("hc.settings.thumbClear")->click(); Dialog.reject(); }
        QCOMPARE(Settings.Read(hc::settings::Section::General), Before);
        { QMainOpts Dialog(main_); Dialog.SetListView(View); Dialog.show(); QTest::qWait(80); Dialog.LoadSettings();
          QCOMPARE(Dialog.findChild<QCheckBox*>("hc.thumbs.previewInverseCursor")->isChecked(), Dialog.findChild<QCheckBox*>("hc.settings.inverseCursor")->isChecked());
          QCOMPARE(Dialog.findChild<QCheckBox*>("hc.thumbs.previewInverseSelection")->isChecked(), Dialog.findChild<QCheckBox*>("hc.settings.inverseSelection")->isChecked());
          Dialog.findChild<QComboBox*>("hc.settings.thumbSize")->setCurrentIndex(2); Dialog.findChild<QComboBox*>("hc.settings.thumbLines")->setCurrentIndex(2);
          Dialog.findChild<QComboBox*>("hc.settings.thumbInfo")->setCurrentIndex(3); Dialog.findChild<QComboBox*>("hc.settings.thumbFit")->setCurrentIndex(1);
          Dialog.findChild<QSpinBox*>("hc.settings.thumbThreads")->setValue(3); Dialog.findChild<QSpinBox*>("hc.settings.thumbCacheMb")->setValue(8);
          Dialog.findChild<QCheckBox*>("hc.settings.thumbAuto")->setChecked(true); Dialog.findChild<QSpinBox*>("hc.settings.thumbPercent")->setValue(60);
          Dialog.findChild<QPushButton*>("hc.settings.thumbClear")->click(); QVERIFY(Dialog.SaveSettings());
          auto* Page = static_cast<hc::ui::ThumbnailSettingsPage*>(Dialog.findChild<QWidget*>("hc.settings.thumbnails")); QVERIFY(Page); QVERIFY(!Page->ClearRequested());
          const auto Artifacts = qEnvironmentVariable("HC_UI_ARTIFACT_DIR"); if (!Artifacts.isEmpty()) { Dialog.findChild<QListWidget*>("lstOptCate")->setCurrentRow(3); QTest::qWait(50); QVERIFY(Dialog.grab().save(QDir(Artifacts).filePath("r4-thumbnail-settings.png"))); }
          {
              Dialog.findChild<QListWidget*>("lstOptCate")->setCurrentRow(3); Dialog.raise(); Dialog.activateWindow(); QTRY_VERIFY(Dialog.isActiveWindow());
              auto* SizeButton = Page->findChild<QPushButton*>("hc.settings.thumbSize.choice1"); SizeButton->setFocus(); QTest::keyClick(SizeButton, Qt::Key_Space); QCOMPARE(Page->Save()["size"].toInt(), 96);
              auto* Preview = Page->findChild<fm::filelist::ThumbnailView*>("hc.settings.thumbPreview"); auto* List = Preview->listView();
              Preview->setCursorRow(0); List->setFocus(); QTest::keyClick(List, Qt::Key_Right); QCOMPARE(Preview->cursorRow(), 1);
              const bool Marked = List->currentIndex().data(fm::filelist::MarkedRole).toBool(); QTest::keyClick(List, Qt::Key_Insert); QCOMPARE(List->model()->index(1, List->modelColumn()).data(fm::filelist::MarkedRole).toBool(), !Marked);
              auto& Theme = fm::style::ThemeManager::instance(); const auto OldScheme = Theme.scheme(); const auto RestoreTheme = qScopeGuard([&] { Theme.setScheme(OldScheme); });
              auto* Scroll = Page->findChild<QScrollArea*>("hc.thumbs.pageScroll");
              for (const bool Dark : {false, true}) {
                  Theme.setScheme(Dark ? fm::style::ThemeManager::Scheme::Dark : fm::style::ThemeManager::Scheme::Light);
                  for (const QSize Size : {QSize(1380, 850), QSize(900, 650)}) {
                      Dialog.resize(Size); QTest::qWait(80); const auto Stable = Dialog.size();
                      QCOMPARE(Scroll->horizontalScrollBar()->maximum(), 0);
                      for (int I = 0; I < 4; ++I) { Dialog.move(Dialog.pos() + QPoint(I % 2 ? -8 : 8, 0)); QCoreApplication::processEvents(); QCOMPARE(Dialog.size(), Stable); }
                      Scroll->verticalScrollBar()->setValue(0); List->scrollToTop(); QTest::qWait(30);
                      const QString Name = QString("thumbnail-%1-%2").arg(Dark ? "dark" : "light").arg(Size.width());
                      if (!Artifacts.isEmpty()) QVERIFY(Dialog.grab().save(QDir(Artifacts).filePath(Name + ".png")));
                      Scroll->verticalScrollBar()->setValue(Scroll->verticalScrollBar()->maximum()); List->scrollToBottom(); QTest::qWait(30);
                      if (!Artifacts.isEmpty()) QVERIFY(Dialog.grab().save(QDir(Artifacts).filePath(Name + "-bottom.png")));
                  }
              }
              if (!Artifacts.isEmpty()) { QFile Dpi(QDir(Artifacts).filePath("thumbnail-dpi.txt")); QVERIFY(Dpi.open(QIODevice::WriteOnly)); Dpi.write(QByteArray::number(Dialog.devicePixelRatioF())); }
          }
          Dialog.reject(); }
        const auto Saved = Settings.Read(hc::settings::Section::General).toObject()["thumbnails"].toObject();
        QCOMPARE(Saved["size"].toInt(), 160); QCOMPARE(Saved["nameLines"].toInt(), 0); QCOMPARE(Saved["threads"].toInt(), 3); QCOMPARE(Saved["cacheMb"].toInt(), 8); QVERIFY(Saved["fill"].toBool());
        QVERIFY(main_->SaveWorkspace()); QProcess Restart; auto Environment = QProcessEnvironment::systemEnvironment();
        Environment.insert("HC_THUMBNAIL_RESTART", "1"); Environment.insert("HC_TEST_CONFIG_DIR", QFileInfo(Settings.FileName()).absolutePath()); Restart.setProcessEnvironment(Environment);
        Restart.start(QCoreApplication::applicationFilePath(), {"thumbnailRestartUi", "-o", "thumbnail-restart.txt,txt"}); QVERIFY(Restart.waitForStarted(5000));
        QTRY_VERIFY_WITH_TIMEOUT(Restart.state() == QProcess::NotRunning, 25000); QCOMPARE(Restart.exitCode(), 0);
        { hc::ui::ThumbnailSettingsPage Page(main_); const QJsonObject Future{{"version", 99}, {"opaque", "preserve"}}; Page.Load(Future); QVERIFY(!Page.isEnabled()); QCOMPARE(Page.Save(), Future); }
    }
    void thumbnailRestartUi()
    {
        if (!qEnvironmentVariableIsSet("HC_THUMBNAIL_RESTART")) QSKIP("Child-process restart check");
        auto* View = static_cast<CGridView*>(panels_.front()->GetFocusView(panels_.front()->CurrentTabIndex())); QTRY_VERIFY(View->IsThumbnailMode()); QCOMPARE(View->ListMode(), QString("thumbnails"));
        const auto Settings = hc::ui::ReadThumbnailSettings(hc::settings::AppSettings().Read(hc::settings::Section::General).toObject()["thumbnails"].toObject());
        QCOMPARE(Settings.appearance_.size, 160); QCOMPARE(Settings.appearance_.nameLines, 0); QCOMPARE(Settings.threads_, 3); QCOMPARE(Settings.cacheMb_, 8); QVERIFY(Settings.appearance_.fill);
    }

    void panelBreadcrumbUi()
    {
        using hc::ui::PanelPathBar;
        QWidget Window;
        Window.resize(1100, 90);
        PanelPathBar Bar(&Window); Bar.setGeometry(10, 10, 1050, 26);
        QLineEdit Other(&Window); Other.setGeometry(10, 50, 300, 25);
        Window.show(); QVERIFY(QTest::qWaitForWindowExposed(&Window));
        Bar.setFullPath("D:\\Clouds\\QSync");
        QSignalSpy Navigated(&Bar, &fm::ui::BreadcrumbBar::pathEntered);
        QSignalSpy Activated(&Bar, &PanelPathBar::activated);
        const auto Root = QPoint(12, Bar.height() / 2);
        const auto Hover = [&] {
            QMouseEvent Move(QEvent::MouseMove, QPointF(Root), QPointF(Bar.mapToGlobal(Root)), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(&Bar, &Move);
        };
        const auto Leave = [&] { QEvent Event(QEvent::Leave); QApplication::sendEvent(&Bar, &Event); };
        QTest::mouseMove(&Other, Other.rect().center()); QTest::qWait(40);
        Leave();
        const auto Idle = Bar.grab().toImage();
        QTest::mouseMove(&Bar, Root); Hover(); QTest::qWait(20);
        QVERIFY(Idle != Bar.grab().toImage());
        QTest::mouseMove(&Other, Other.rect().center()); QTest::qWait(20);
        Leave();
        QTRY_COMPARE(Idle, Bar.grab().toImage());
        QTest::mouseClick(&Bar, Qt::LeftButton, Qt::NoModifier, Root);
        QVERIFY(!Activated.isEmpty());
        QTRY_COMPARE(Navigated.size(), 1);
        QCOMPARE(Navigated.takeFirst().at(0).toString(), QString("D:\\"));

        // The first click of a double click must not navigate to its ancestor.
        QTest::mouseClick(&Bar, Qt::LeftButton, Qt::NoModifier, Root);
        QTest::mouseDClick(&Bar, Qt::LeftButton, Qt::NoModifier, Root);
        auto* Editor = Bar.findChild<QLineEdit*>("hc.panelPath.editor"); QVERIFY(Editor);
        QTRY_VERIFY(Editor->isVisible()); QCOMPARE(Editor->selectedText(), QString("D:\\Clouds\\QSync"));
        QTest::qWait(QApplication::doubleClickInterval() + 30); QVERIFY(Navigated.isEmpty());
        QTest::keyClick(Editor, Qt::Key_Escape); QTRY_VERIFY(!Editor->isVisible());
        QTest::mouseClick(&Bar, Qt::LeftButton, Qt::NoModifier, QPoint(Bar.width() - 15, 12));
        QTRY_VERIFY(Editor->isVisible()); QCOMPARE(Editor->selectedText(), QString("D:\\Clouds\\QSync"));
        Editor->setText("D:\\Other"); QTest::keyClick(Editor, Qt::Key_Return);
        QTRY_COMPARE(Navigated.size(), 1); QCOMPARE(Navigated.takeFirst().at(0).toString(), QString("D:\\Other"));
        QVERIFY(!Editor->isVisible());

        Bar.resize(130, 26); QTest::mouseMove(&Other, Other.rect().center());
        QTest::mouseClick(&Bar, Qt::LeftButton, Qt::NoModifier, Root);
        auto* Overflow = Bar.findChild<QMenu*>("hc.panelPath.folders"); QVERIFY(Overflow);
        QTRY_VERIFY(Overflow->isVisible()); QVERIFY(!Overflow->actions().isEmpty());
        QCOMPARE(Overflow->actions().front()->data().toString(), QString("D:\\"));
        Overflow->close();

        // UNC roots stay intact even when the earlier ancestors are folded.
        Bar.setFullPath("\\\\server\\share\\folder\\child");
        QTest::mouseClick(&Bar, Qt::LeftButton, Qt::NoModifier, Root);
        QMenu* Unc = nullptr;
        for (auto* Menu : Bar.findChildren<QMenu*>("hc.panelPath.folders")) if (Menu->isVisible()) Unc = Menu;
        QVERIFY(Unc); QCOMPARE(Unc->actions().front()->data().toString(), QString("\\\\server\\share")); Unc->close();

        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        QVERIFY(QDir(Directory.path()).mkdir("child & folder")); QVERIFY(QDir(Directory.path()).mkdir("zeta"));
        Bar.resize(1050, 26); Bar.setFullPath(QDir::toNativeSeparators(Directory.path()));
        // Down opens the current folder's separator menu without pointer input.
        Bar.setFocus(); QTest::keyClick(&Bar, Qt::Key_Down);
        QMenu* Folders = nullptr;
        for (auto* Menu : Bar.findChildren<QMenu*>("hc.panelPath.folders")) if (Menu->isVisible()) Folders = Menu;
        QVERIFY(Folders);
        const auto Child = QDir::toNativeSeparators(Directory.filePath("child & folder"));
        const auto FindChild = [&]() -> QAction* { for (auto* Action : Folders->actions()) if (Action->data().toString() == Child) return Action; return nullptr; };
        QTRY_VERIFY(FindChild());
        FindChild()->trigger();
        QTRY_COMPARE(Navigated.size(), 1); QCOMPARE(Navigated.takeFirst().at(0).toString(), Child);

        // An old menu cannot publish into a replacement location.
        Bar.setFocus(); QTest::keyClick(&Bar, Qt::Key_Down);
        Bar.setFullPath("D:\\Clouds\\QSync"); QTest::qWait(50);
        for (auto* Menu : Bar.findChildren<QMenu*>("hc.panelPath.folders")) QVERIFY(!Menu->isVisible());
        Bar.SetBreadcrumbsEnabled(false);
        QTest::mouseClick(&Bar, Qt::LeftButton, Qt::NoModifier, Root);
        QTRY_VERIFY(Editor->isVisible()); QTest::keyClick(Editor, Qt::Key_Escape);
        Bar.SetBreadcrumbsEnabled(true);
        Bar.setFullPath(QDir::toNativeSeparators(Directory.filePath("missing")));
        Bar.setFocus(); QTest::keyClick(&Bar, Qt::Key_Down);
        QMenu* Missing = nullptr;
        for (auto* Menu : Bar.findChildren<QMenu*>("hc.panelPath.folders")) if (Menu->isVisible()) Missing = Menu;
        QVERIFY(Missing);
        QTRY_VERIFY(Missing->actions().front()->text().contains("win32:"));
        QVERIFY(!Missing->actions().front()->isEnabled()); Missing->close();
    }

    void panelBreadcrumbNavigationUi()
    {
        using hc::ui::PanelPathBar;
        const auto Workspace = main_->CaptureWorkspace();
        const auto Restore = qScopeGuard([&] { main_->RestoreWorkspace(Workspace); });
        main_->resize(1400, 900); main_->show();
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        QVERIFY(QDir(Directory.path()).mkpath("parent/child"));
        auto* Panel = main_->findChild<CmpPanel*>("cmpRightPanel"); QVERIFY(Panel);
        const auto Target = QDir::toNativeSeparators(Directory.filePath("parent/child"));
        Panel->ChangeCurrentPath(Target, {}); QTRY_COMPARE(Panel->CurrentPath(), Target);
        auto* Bar = Panel->findChild<PanelPathBar*>("hc.panelPath"); QVERIFY(Bar);
        main_->findChild<CmpPanel*>("cmpLeftPanel")->sig_NotifyPanelActivated(); QVERIFY(!Bar->IsPaneActive());
        Bar->EditPath(); QVERIFY(Bar->IsPaneActive());
        auto* Editor = Bar->findChild<QLineEdit*>("hc.panelPath.editor"); QVERIFY(Editor);
        const auto Parent = QDir::toNativeSeparators(Directory.filePath("parent"));
        Editor->setText(Parent); QTest::keyClick(Editor, Qt::Key_Return);
        QTRY_COMPARE(Panel->CurrentPath(), Parent);
        Bar->setFocus(); QTest::keyClick(Bar, Qt::Key_Down);
        auto* Menu = Bar->findChild<QMenu*>("hc.panelPath.folders"); QVERIFY(Menu);
        const auto ChildAction = [&]() -> QAction* { for (auto* Action : Menu->actions()) if (Action->data().toString() == Target) return Action; return nullptr; };
        QTRY_VERIFY(ChildAction()); ChildAction()->trigger();
        QTRY_COMPARE(Panel->CurrentPath(), Target);
        const auto Dir = QCoreApplication::applicationDirPath();
        QTest::mouseMove(main_, QPoint(5, main_->height() - 5)); QTest::qWait(20);
        QEvent Leave(QEvent::Leave); QApplication::sendEvent(Bar, &Leave);
        QVERIFY(Bar->grab().save(Dir + "/breadcrumb-idle.png"));
        QTest::mouseMove(Bar, QPoint(12, Bar->height() / 2)); QTest::qWait(20);
        QMouseEvent Move(QEvent::MouseMove, QPointF(12, Bar->height() / 2), QPointF(Bar->mapToGlobal(QPoint(12, Bar->height() / 2))), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(Bar, &Move);
        QVERIFY(Bar->grab().save(Dir + "/breadcrumb-hover.png"));
    }

    void activePanelAppearanceUi()
    {
        const auto Workspace = main_->CaptureWorkspace();
        const auto Restore = qScopeGuard([&] { main_->RestoreWorkspace(Workspace); });
        auto* Left = main_->findChild<CmpPanel*>("cmpLeftPanel"); auto* Right = main_->findChild<CmpPanel*>("cmpRightPanel");
        auto* LeftPath = Left->findChild<fm::ui::BreadcrumbBar*>("hc.panelPath");
        auto* RightPath = Right->findChild<fm::ui::BreadcrumbBar*>("hc.panelPath");
        QVERIFY(LeftPath && RightPath);
        Left->sig_NotifyPanelActivated();
        QVERIFY(LeftPath->IsPaneActive()); QVERIFY(!RightPath->IsPaneActive());
        const auto Geometry = LeftPath->geometry();
        Right->sig_NotifyPanelActivated();
        QVERIFY(RightPath->IsPaneActive()); QVERIFY(!LeftPath->IsPaneActive());
        QCOMPARE(LeftPath->geometry(), Geometry);
        QCOMPARE(main_->CaptureWorkspace().activePane_, 1);
        QCOMPARE(QString::fromStdString(TyStCommandMgr::GetInstance()->Bridge()->Capture().location_.root_), Right->CurrentPath());
        { QDialog Dialog(main_); QLineEdit Edit(&Dialog); Dialog.show(); Edit.setFocus(); QTest::qWait(30); QVERIFY(RightPath->IsPaneActive()); }
    }


    void r14ThemeRestartUi()
    {
        if (!qEnvironmentVariableIsSet("HC_R14_THEME_RESTART")) QSKIP("Only runs as a theme restart child");
        using namespace fm::style;
        auto& Settings = hc::settings::AppSettings();
        QCOMPARE(Settings.Read(hc::settings::Section::General).toObject()["themeMode"].toString(), QString("dark"));
        QCOMPARE(ThemeManager::instance().effectiveVariant(), Variant::Dark);
        QCOMPARE(qApp->palette().color(QPalette::Base), ThemeManager::instance().colors()[Token::Surface]);
        QMainOpts Dialog(main_); Dialog.show(); QTest::qWait(80); Dialog.LoadSettings();
        QCOMPARE(Dialog.findChild<QComboBox*>("hc.settings.theme")->currentIndex(), 1);
        QCOMPARE(Dialog.findChild<QLineEdit*>("hc.settings.search")->palette().color(QPalette::Base), ThemeManager::instance().colors()[Token::Surface]);
        const auto Dir = qEnvironmentVariable("HC_UI_ARTIFACT_DIR");
        if (!Dir.isEmpty()) QVERIFY(Dialog.grab().save(QDir(Dir).filePath("r14-theme-restart-dark.png")));
        Dialog.reject();
    }

    void r14ThemeConsistencyUi()
    {
        using namespace hc::settings; using namespace fm::style;
        auto& Settings = AppSettings(); const auto Original = Settings.Object();
        auto* Colors = TyStColorSchemeMgr::GetInstance();
        const auto Restore = qScopeGuard([&] { Settings.DiscardEdit(); Settings.Replace(Original); Settings.Commit(); Colors->Refresh(); emit Colors->sigColorSchemeChanged(Colors->GetColorScheme(Colors->GetCurrentColorScheme())); });
        auto General = Settings.Read(Section::General).toObject(); General["themeMode"] = "light"; Settings.Put(Section::General, General);
        auto Appearance = BuiltInAppearance(); auto Schemes = Appearance["schemes"].toObject();
        auto Dark = Schemes[QStringLiteral("기본 다크")].toObject(); const auto CustomColors = Dark["colors"].toObject();
        Dark["dark"] = false; Dark["future"] = "keep";
        Dark["colorOverrides"] = QJsonObject{{"dialogText", true}, {"dialogBackground", true}, {"listText", true}, {"listBackground", true}, {"listCursor", true}, {"listSelection", true}};
        Schemes[QStringLiteral("기본 다크")] = Dark; Appearance["schemes"] = Schemes;
        Settings.Put(Section::Appearance, Appearance); QVERIFY(Settings.Commit());
        Colors->Refresh(); emit Colors->sigColorSchemeChanged(Colors->GetColorScheme(Colors->GetCurrentColorScheme()));
        QMainOpts Dialog(main_); Dialog.show(); QTest::qWait(80); Dialog.LoadSettings();
        const auto Save = [&](const QString& Name) { const auto Dir = qEnvironmentVariable("HC_UI_ARTIFACT_DIR"); return Dir.isEmpty() || Dialog.grab().save(QDir(Dir).filePath(Name)); };
        auto* Mode = Dialog.findChild<QComboBox*>("hc.settings.theme"); QVERIFY(Mode);
        QTest::mouseClick(Dialog.findChild<fm::ui::ThemeModeCard*>("hc.settings.themeCard1"), Qt::LeftButton);
        QCOMPARE(Mode->currentIndex(), 1);
        Dialog.findChild<QPushButton*>("btnApply")->click(); QTest::qWait(40);
        QCOMPARE(ThemeManager::instance().effectiveVariant(), Variant::Dark);
        QCOMPARE(qApp->palette().color(QPalette::Base), ThemeManager::instance().colors()[Token::Surface]);
        auto* Search = Dialog.findChild<QLineEdit*>("hc.settings.search");
        QCOMPARE(Search->palette().color(QPalette::Base), ThemeManager::instance().colors()[Token::Surface]);
        QVERIFY(Search->palette().color(QPalette::Base) != QColor(Qt::white));
        QCOMPARE(Settings.Read(Section::General).toObject()["themeMode"].toString(), QString("dark"));
        const auto StoredDark = Settings.Read(Section::Appearance).toObject()["schemes"].toObject()[QStringLiteral("기본 다크")].toObject();
        QVERIFY(StoredDark["dark"].toBool()); QCOMPARE(StoredDark["future"].toString(), QString("keep"));
        for (auto It = CustomColors.begin(); It != CustomColors.end(); ++It)
            QCOMPARE(QColor(StoredDark["colors"].toObject()[It.key()].toString()), QColor(It.value().toString()));
        QVERIFY(Save("r14-theme-general-dark.png"));
        auto* Categories = Dialog.findChild<QListWidget*>("lstOptCate");
        for (int Row = 0; Row < Categories->count(); ++Row) {
            Categories->setCurrentRow(Row); QTest::qWait(5);
            QCOMPARE(Categories->palette().color(QPalette::Base), ThemeManager::instance().colors()[Token::Surface]);
        }
        Categories->setCurrentRow(7); QVERIFY(Save("r14-theme-keyboard-dark.png"));
        Categories->setCurrentRow(0);
        Mode->setCurrentIndex(0); QVERIFY(Dialog.SaveSettings());
        QCOMPARE(ThemeManager::instance().effectiveVariant(), Variant::Light);
        Mode->setCurrentIndex(2); QVERIFY(Dialog.SaveSettings());
        QCOMPARE(ThemeManager::instance().scheme(), ThemeManager::Scheme::System);
        Mode->setCurrentIndex(1); QVERIFY(Dialog.SaveSettings());
        QCOMPARE(ThemeManager::instance().effectiveVariant(), Variant::Dark);
        Dialog.reject();
        QProcess Restart; auto Environment = QProcessEnvironment::systemEnvironment();
        Environment.insert("HC_R14_THEME_RESTART", "1"); Environment.insert("HC_TEST_CONFIG_DIR", QFileInfo(Settings.FileName()).absolutePath());
        Restart.setProcessEnvironment(Environment); Restart.start(QCoreApplication::applicationFilePath(), {"r14ThemeRestartUi", "-o", "r14-theme-restart.txt,txt"});
        QVERIFY(Restart.waitForStarted(5000)); QVERIFY(Restart.waitForFinished(30000)); QCOMPARE(Restart.exitCode(), 0);
        QMainOpts Cancel(main_); Cancel.show(); QTest::qWait(60); Cancel.LoadSettings();
        Cancel.findChild<QComboBox*>("hc.settings.theme")->setCurrentIndex(0); Cancel.reject();
        QCOMPARE(Settings.Read(Section::General).toObject()["themeMode"].toString(), QString("dark"));
        QCOMPARE(ThemeManager::instance().effectiveVariant(), Variant::Dark);
    }

    void r14SettingsPolishUi()
    {
        using namespace hc::settings;
        auto& Settings = AppSettings(); const auto Original = Settings.Object();
        auto* Colors = TyStColorSchemeMgr::GetInstance(); const auto Scheme = Colors->GetColorScheme(Colors->GetCurrentColorScheme());
        const auto Restore = qScopeGuard([&] { Settings.DiscardEdit(); Settings.Replace(Original); Settings.Commit(); Colors->Refresh(); emit Colors->sigColorSchemeChanged(Scheme); });
        const auto MakeGroup = [](const QString& Id, const QString& Name, const QString& Color) {
            return QJsonObject{{"id", Id}, {"name", Name}, {"enabled", true}, {"join", "any"},
                {"conditions", QJsonArray{QJsonObject{{"type", "extension"}, {"value", "exe"}}}},
                {"light", QJsonObject{{"foreground", Color}}}, {"future", Id + ".retained"}};
        };
        auto Fixture = FileColorRules::Empty(); Fixture["groups"] = QJsonArray{MakeGroup("one", "실행파일", "#D00000"), MakeGroup("two", "도구", "#008577"), MakeGroup("three", "기타 실행파일", "#0055CC")};
        QMainOpts Dialog(main_); Dialog.show(); QTest::qWait(80); Dialog.LoadSettings();
        auto* Categories = Dialog.findChild<QListWidget*>("lstOptCate"); QVERIFY(Categories);
        QCOMPARE(Categories->count(), 14);
        for (int Row = 0; Row < Categories->count(); ++Row) {
            QVERIFY(!Categories->item(Row)->text().contains("기존 파일집합"));
            Categories->setCurrentRow(Row); QCoreApplication::processEvents();
            auto* Stack = Dialog.findChild<QStackedWidget*>("stkOpts");
            QCOMPARE(Stack->currentIndex(), Categories->item(Row)->data(Qt::UserRole).toInt());
        }
        QVERIFY(!Dialog.findChild<QWidget*>("pgFileSet"));
        auto* Chrome = Dialog.findChild<fm::ui::WindowChrome*>("hc.settings.windowChrome"); QVERIFY(Chrome && Chrome->isVisible());
        QVERIFY(Dialog.findChild<QWidget*>("frmTitleBar")->isHidden());
        auto* Page = static_cast<hc::ui::FileColorRulesPage*>(Dialog.findChild<QWidget*>("hc.settings.fileColors"));
        Page->Load(Fixture); Categories->setCurrentRow(4); QTest::qWait(40);
        auto* List = Page->findChild<fm::ui::ReorderListWidget*>("hc.rules.groups"); QVERIFY(List);
        auto* Table = Page->findChild<QTableWidget*>("hc.rules.conditions"); QVERIFY(Table->verticalHeader()->isHidden());
        List->setCurrentRow(0); Page->findChild<QLineEdit*>("hc.rules.name")->setText("실행파일 · 편집 유지");
        QCoreApplication::processEvents(); QCOMPARE(List->horizontalScrollBar()->maximum(), 0);
        const auto Save = [&](const QString& Name) { const auto Dir = qEnvironmentVariable("HC_UI_ARTIFACT_DIR"); if (Dir.isEmpty()) return true; return Dialog.grab().save(QDir(Dir).filePath(Name)); };
        QVERIFY(Save("r14-settings-before-drag.png"));
        auto* Item = List->item(0);
        const auto Send = [&](QEvent::Type Type, QPoint Point, Qt::MouseButton Button, Qt::MouseButtons Buttons) {
            QMouseEvent Event(Type, Point, List->viewport()->mapToGlobal(Point), Button, Buttons, Qt::NoModifier);
            QApplication::sendEvent(List->viewport(), &Event);
        };
        const auto Target = List->visualItemRect(List->item(2)).bottomLeft() + QPoint(15, -1);
        Send(QEvent::MouseButtonPress, List->HandleRect(0).center(), Qt::LeftButton, Qt::LeftButton);
        Send(QEvent::MouseMove, Target, Qt::NoButton, Qt::LeftButton);
        QCoreApplication::processEvents(); QVERIFY(Save("r14-settings-dragging.png"));
        Send(QEvent::MouseButtonRelease, Target, Qt::LeftButton, Qt::NoButton);
        QCOMPARE(List->item(2), Item); QCOMPARE(List->currentItem(), Item);
        Page->findChild<QPushButton*>("hc.rules.moveUp")->click(); QCOMPARE(List->item(1), Item);
        Page->findChild<QPushButton*>("hc.rules.moveDown")->click(); QCOMPARE(List->item(2), Item);
        QVERIFY(Item->text().startsWith("3. "));
        const auto Groups = Page->Document()["groups"].toArray();
        QCOMPARE(Groups[0].toObject()["id"].toString(), QString("two"));
        QCOMPARE(Groups[2].toObject()["name"].toString(), QString("실행파일 · 편집 유지"));
        QCOMPARE(Groups[2].toObject()["future"].toString(), QString("one.retained"));
        FileColorRules Rules; Rules.Load(Page->Document());
        QCOMPARE(Rules.Evaluate("app.exe", 0, 0, false).foreground_, QColor("#008577"));
        QVERIFY(Dialog.SaveSettings());
        Repository Reload(QFileInfo(Settings.FileName()).absolutePath());
        QCOMPARE(Reload.Read(Section::Options).toObject()["fileColorRules"].toObject()["groups"].toArray(), Groups);
        List->item(2)->setCheckState(Qt::Unchecked);
        QVERIFY(!Page->Document()["groups"].toArray()[2].toObject()["enabled"].toBool());
        QTest::keyClick(List, Qt::Key_Up, Qt::AltModifier); QCOMPARE(List->currentRow(), 1);
        QCOMPARE(Page->findChild<QLineEdit*>("hc.rules.name")->text(), QString("실행파일 · 편집 유지"));
        // Cancel discards later priority/check changes after Apply.
        List->item(1)->setCheckState(Qt::Checked); QVERIFY(List->MoveRow(1, 2));
        QVERIFY(Save("r14-settings-light.png"));
        Dialog.findChild<QComboBox*>("hc.settings.theme")->setCurrentIndex(1); QVERIFY(Dialog.SaveSettings()); QTest::qWait(30);
        QVERIFY(Save("r14-settings-dark.png"));
        QVERIFY(List->MoveRow(0, 1)); Page->findChild<QLineEdit*>("hc.rules.name")->setText("취소할 편집");
        Dialog.reject();
        QCOMPARE(Settings.Read(Section::Options).toObject()["fileColorRules"].toObject()["groups"].toArray(), Groups);
    }

    void r21TransferProgressUi()
    {
        using namespace hc::operations; using namespace hc::settings;
        auto& Settings=AppSettings(); const auto Original=Settings.Object(); const auto Restore=qScopeGuard([&]{Settings.DiscardEdit();Settings.Replace(Original);Settings.Commit();});
        auto Options=Settings.Read(Section::Options).toObject(); Options.remove("jobCloseOnSuccess"); Options["saveJobProgress"]=false; Options["jobDetailed"]=true; QVERIFY(Settings.Put(Section::Options,Options)); QVERIFY(Settings.Commit());
        struct Control { std::atomic<bool> commit_{false},unknown_{false},finish_{false}; };
        class Task final : public IJobTask {
        public:
            explicit Task(std::shared_ptr<Control> Control) : control_(std::move(Control)) {}
            JobStep Advance(JobStepContext) override {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                JobStep Step{control_->finish_?JobState::Completed:control_->commit_?JobState::Committing:JobState::Running};
                const auto Done=std::min<uint64_t>(++count_*65536,16*1024*1024);
                Step.progress_=JobProgress{Done,control_->unknown_?std::optional<uint64_t>{}:std::optional<uint64_t>{16*1024*1024},"sample.bin",Done,
                    control_->unknown_?std::optional<uint64_t>{}:std::optional<uint64_t>{16*1024*1024},control_->unknown_?std::optional<uint64_t>{}:std::optional<uint64_t>{2}};
                return Step;
            }
            std::vector<hc::storage::Error> Finalize(JobState) override { return {}; }
        private:
            std::shared_ptr<Control> control_; uint64_t count_=0;
        };
        QWidget Window; Window.setAttribute(Qt::WA_QuitOnClose,false);
        auto ProgressFont = Window.font(); ProgressFont.setPointSize(12); Window.setFont(ProgressFont);
        FileJobUi Jobs(&Window,std::make_shared<hc::commands::EventHub>());
        auto Control=std::make_shared<struct Control>(); PreparedJob Work; Work.request_.metadata_={"","source.7z","D:/Qt/output","extract; shared-folder"}; Work.task_=std::make_unique<Task>(Control);
        auto Started=Jobs.Submit(std::move(Work),QStringLiteral("파일 압축 해제")); QVERIFY(Started.HasValue());
        auto* Dialog=Window.findChild<QDialog*>("hc.fileJobs"); auto* Pause=Window.findChild<QPushButton*>("hc.jobs.pause"); auto* Graph=Window.findChild<fm::ui::TransferGraph*>("hc.jobs.graph");
        auto* AutoClose=Window.findChild<QCheckBox*>("hc.jobs.closeOnSuccess"); auto* Metrics=Window.findChild<QLabel*>("hc.jobs.metrics");
        auto* Details=Window.findChild<QPlainTextEdit*>("hc.jobs.details"); auto* Bar=Window.findChild<QProgressBar*>();
        QVERIFY(Dialog && Pause && Graph && AutoClose && Metrics && Details && Bar); QVERIFY(AutoClose->isChecked());
        QTRY_VERIFY(Graph->bytesDone()>0); QTRY_VERIFY(Bar->maximum()>0 && Bar->value()>0); QVERIFY(Graph->totalBytes()==16*1024*1024);
        QVERIFY(Metrics->text().contains("MiB")); QVERIFY(!Metrics->text().contains(QStringLiteral("미상"))); QVERIFY(!Details->isVisible());
        auto* Header=Dialog->findChild<fm::ui::DialogHeader*>(); QVERIFY(Header); QVERIFY(Header->subtitle().contains("D:/Qt/output")); QVERIFY(!Header->subtitle().contains("hc-job-"));
        auto* Toggle = Dialog->findChild<QPushButton*>("hc.jobs.toggleDetails"); QVERIFY(Toggle);
        QComboBox* Axis = nullptr;
        for (auto* Combo : Dialog->findChildren<QComboBox*>()) if (Combo->itemText(0) == QStringLiteral("진행률 축")) Axis = Combo;
        QVERIFY(Axis);
        if (!Graph->isVisible()) Toggle->click();
        QTest::qWait(200);
        QVERIFY2(Graph->geometry().bottom() < Toggle->geometry().top(), qPrintable(QString("graph=%1,%2 %3x%4 toggle=%5,%6 %7x%8")
            .arg(Graph->x()).arg(Graph->y()).arg(Graph->width()).arg(Graph->height()).arg(Toggle->x()).arg(Toggle->y()).arg(Toggle->width()).arg(Toggle->height())));
        QVERIFY(Graph->geometry().bottom() < Axis->geometry().top());
        QVERIFY(Toggle->geometry().bottom() < AutoClose->geometry().top());
        QVERIFY(Axis->geometry().bottom() < AutoClose->geometry().top());
        QVERIFY(SaveR9Dialog(*Dialog, "progress-graph-expanded"));
        Toggle->click(); QTRY_VERIFY(!Graph->isVisible()); QVERIFY(!Axis->isVisible());
        QVERIFY(SaveR9Dialog(*Dialog, "progress-graph-collapsed"));
        Toggle->click(); QTRY_VERIFY(Graph->isVisible() && Axis->isVisible());
        QTRY_VERIFY(Graph->geometry().bottom() < Toggle->geometry().top());
        Axis->setCurrentIndex(1); QCOMPARE(Graph->axis(), fm::ui::TransferGraph::Time);
        for (const int Points : {9, 12, 16}) {
            auto Font = Dialog->font(); Font.setPointSize(Points); Dialog->setFont(Font);
            Dialog->resize(700, 350); QTest::qWait(100);
            QTRY_VERIFY(Graph->geometry().bottom() < Toggle->geometry().top());
            QVERIFY(Graph->geometry().bottom() < Axis->geometry().top());
            QVERIFY(Toggle->geometry().bottom() < AutoClose->geometry().top());
            QVERIFY(Axis->geometry().bottom() < AutoClose->geometry().top());
            QVERIFY(Dialog->rect().contains(Pause->geometry()));
        }
        QVERIFY(SaveR9Dialog(*Dialog,"r21-progress-known"));
        Control->commit_=true; QTRY_COMPARE(Jobs.Scheduler().Snapshot(Started.Value().id_).Value().state_,JobState::Committing); QTest::qWait(250); QVERIFY(Pause->isEnabled());
        Pause->click(); QTRY_VERIFY(Jobs.Scheduler().Snapshot(Started.Value().id_).Value().pauseRequested_); Control->commit_=false;
        QTRY_COMPARE(Jobs.Scheduler().Snapshot(Started.Value().id_).Value().state_,JobState::Paused); QTRY_VERIFY(Graph->isPaused());
        QVERIFY(SaveR9Dialog(*Dialog,"r21-progress-paused")); Pause->click(); QTRY_VERIFY(!Graph->isPaused());
        Control->unknown_=true; QTRY_COMPARE(Bar->maximum(),0); QVERIFY(!Metrics->text().contains(QStringLiteral("미상"))); QVERIFY(!Metrics->text().contains(QStringLiteral("예정")));
        QVERIFY(SaveR9Dialog(*Dialog,"r21-progress-preparing"));
        Control->unknown_=false; QTRY_VERIFY(Bar->maximum()>0);
        QTRY_VERIFY(Graph->geometry().bottom() < Toggle->geometry().top());
        QVERIFY(Graph->geometry().bottom() < Axis->geometry().top());
        AutoClose->setChecked(true); Control->finish_=true;
        QTRY_VERIFY(!Jobs.IsUiPending(Started.Value().id_)); QTRY_VERIFY(!Dialog->isVisible());
        fm::ui::TransferGraph Rolling; Rolling.resize(760,170); Rolling.setAxis(fm::ui::TransferGraph::Time); Rolling.start(100000000);
        for(qint64 Ms=0;Ms<=55000;Ms+=500) Rolling.addSample(Ms*1000+(Ms>30000?10000000:0),Ms);
        const auto Span=Rolling.timeWindowEndMs()-Rolling.timeWindowStartMs(); QCOMPARE(Span,60000ll); QCOMPARE(Rolling.timeWindowStartMs(),0ll);
        Rolling.show(); QVERIFY(SaveR9Dialog(Rolling,"r21-graph-time-before"));
        for(qint64 Ms=55500;Ms<=85000;Ms+=500) Rolling.addSample(Ms*1000+10000000,Ms);
        QCOMPARE(Rolling.timeWindowEndMs()-Rolling.timeWindowStartMs(),Span); QVERIFY(Rolling.timeWindowStartMs()>0);
        QVERIFY(SaveR9Dialog(Rolling,"r21-graph-time-after")); Rolling.hide();
    }
    void todoBugDeleteProgressUi()
    {
        using namespace hc::operations; using namespace hc::settings;
        auto& Settings = AppSettings(); const auto Original = Settings.Object();
        const auto Restore = qScopeGuard([&] { Settings.DiscardEdit(); Settings.Replace(Original); Settings.Commit(); });
        auto Options = Settings.Read(Section::Options).toObject(); Options.remove("deleteWhileListing"); Options["enableJobRecovery"] = false;
        Options["jobCloseOnSuccess"] = false; QVERIFY(Settings.Put(Section::Options, Options));
        {
            QMainOpts Dialog(main_); Dialog.LoadSettings(); Dialog.show();
            auto* Nav = Dialog.findChild<QListWidget*>("lstOptCate"); QVERIFY(Nav);
            for (int I = 0; I < Nav->count(); ++I) if (Nav->item(I)->text() == QStringLiteral("파일 작업")) Nav->setCurrentRow(I);
            auto* Tabs = Dialog.findChild<QTabWidget*>("hc.settings.fileOperationsTabs"); QVERIFY(Tabs);
            auto* Page = Dialog.findChild<QWidget*>("hc.settings.fileOperationsDelete"); QVERIFY(Page); Tabs->setCurrentWidget(Page);
            auto* Check = Dialog.findChild<QCheckBox*>("hc.settings.deleteWhileListing"); QVERIFY(Check); QVERIFY(Check->isChecked());
            QTRY_VERIFY(Check->isVisible()); QVERIFY(SaveR9Dialog(Dialog, "delete-settings-default-on"));
            Check->setChecked(false); QVERIFY(Dialog.SaveSettings()); Dialog.accept();
        }
        QVERIFY(!Settings.CommittedObject()["options"].toObject()["deleteWhileListing"].toBool(true));
        { QMainOpts Dialog(main_); Dialog.LoadSettings(); auto* Check = Dialog.findChild<QCheckBox*>("hc.settings.deleteWhileListing");
          QVERIFY(Check && !Check->isChecked()); Dialog.reject(); }
        struct Control { std::atomic<bool> planned_{false}, finish_{false}; };
        class Task final : public IJobTask {
        public:
            explicit Task(std::shared_ptr<Control> Control) : control_(std::move(Control)) {}
            JobStep Advance(JobStepContext) override {
                std::this_thread::sleep_for(std::chrono::milliseconds(20)); ++count_;
                JobStep Step{control_->finish_ ? JobState::Completed : JobState::Preparing}; Step.progress_ = JobProgress{};
                Step.progress_->deletedFiles_ = count_ / 2; Step.progress_->discoveredFiles_ = count_; Step.progress_->discoveredItems_ = count_ + 3;
                if (control_->planned_) Step.progress_->plannedItems_ = count_ + 3;
                return Step;
            }
            std::vector<hc::storage::Error> Finalize(JobState) override { return {}; }
        private: std::shared_ptr<Control> control_; uint64_t count_ = 0;
        };
        QWidget Window; Window.setAttribute(Qt::WA_QuitOnClose, false); FileJobUi Jobs(&Window, std::make_shared<hc::commands::EventHub>());
        auto Control = std::make_shared<struct Control>(); PreparedJob Work; Work.task_ = std::make_unique<Task>(Control);
        Work.request_.metadata_ = {"qtbase", "D:/Qt/한글 경로/qtbase", "hcl11:00:", {}};
        auto Started = Jobs.Submit(std::move(Work), QStringLiteral("삭제 진행 시험")); QVERIFY(Started.HasValue());
        auto* Dialog = Window.findChild<QDialog*>("hc.fileJobs"); auto* Graph = Window.findChild<fm::ui::TransferGraph*>("hc.jobs.graph");
        auto* Metrics = Window.findChild<QLabel*>("hc.jobs.metrics"); auto* Pause = Window.findChild<QPushButton*>("hc.jobs.pause");
        auto* Bar = Window.findChild<QProgressBar*>(); QVERIFY(Dialog && Graph && Metrics && Pause && Bar);
        QTRY_VERIFY(Graph->bytesDone() > 0); QTRY_COMPARE(Bar->maximum(), 0); QCOMPARE(Graph->totalBytes(), 0ll);
        auto* Header = Dialog->findChild<fm::ui::DialogHeader*>(); QVERIFY(Header);
        QTRY_COMPARE(Header->title(), QStringLiteral("삭제 · D:/Qt/한글 경로/qtbase"));
        QVERIFY(Header->subtitle().isEmpty()); QCOMPARE(Header->toolTip(), Header->title());
        QVERIFY(Dialog->windowTitle().contains(Header->title()));
        QVERIFY(Graph->formatValue(12).contains(QStringLiteral("파일/초"))); QVERIFY(!Metrics->text().contains("B/s"));
        QTRY_VERIFY(Metrics->text().contains(QRegularExpression(QStringLiteral("속도 [1-9]"))));
        QVERIFY(Metrics->text().contains(QStringLiteral("확인한 항목"))); QVERIFY(Metrics->text().contains(QStringLiteral("전체 집계 중")));
        QVERIFY(SaveR9Dialog(*Dialog, "delete-discovery-busy"));
        {
            auto& Theme = fm::style::ThemeManager::instance(); const auto OriginalDesign = Theme.design();
            const auto RestoreDesign = qScopeGuard([&] { Theme.setDesign(OriginalDesign); });
            for (const auto Design : {fm::style::Design::Standard, fm::style::Design::Watercolor}) {
                Theme.setDesign(Design); QTest::qWait(80); Bar->setTextVisible(false);
                const auto Before = Bar->grab().toImage(); QTest::qWait(180); const auto After = Bar->grab().toImage();
                QVERIFY2(Before != After, "Indeterminate deletion progress must animate in both designs.");
            }
            Bar->setTextVisible(true);
        }
        Pause->click(); QTRY_COMPARE(Jobs.Scheduler().Snapshot(Started.Value().id_).Value().state_, JobState::Paused);
        QTRY_VERIFY(Bar->maximum() > 0); QVERIFY(Graph->isPaused()); QVERIFY(SaveR9Dialog(*Dialog, "delete-discovery-paused"));
        Pause->click(); QTRY_COMPARE(Bar->maximum(), 0); Control->planned_ = true; QTRY_VERIFY(Bar->maximum() > 0);
        Control->finish_ = true; QTRY_VERIFY(!Jobs.IsUiPending(Started.Value().id_));
        auto CancelControl = std::make_shared<struct Control>(); PreparedJob CancelWork; CancelWork.task_ = std::make_unique<Task>(CancelControl);
        auto CancelJob = Jobs.Submit(std::move(CancelWork), QStringLiteral("목록 확인 취소 시험")); QVERIFY(CancelJob.HasValue());
        QTRY_COMPARE(Bar->maximum(), 0); auto* Cancel = Window.findChild<QPushButton*>("hc.jobs.cancel");
        QVERIFY(Cancel && Cancel->isEnabled()); Cancel->click();
        QTRY_VERIFY(CancelJob.Value().completion_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        QCOMPARE(CancelJob.Value().completion_.get().snapshot_.state_, JobState::Cancelled);
    }
    void r23FileWritesUi()
    {
        using namespace hc::settings; using namespace hc::operations; using namespace hc::storage;
        auto& Settings=AppSettings(); const auto Original=Settings.Object();
        const auto Restore=qScopeGuard([&]{Settings.DiscardEdit();Settings.Replace(Original);Settings.Commit();TyStCommandMgr::GetInstance()->Bridge()->FileJobs()->ApplyDefaults();});
        auto Options=Settings.Read(Section::Options).toObject(); Options.remove("delayedFileWrites"); Options["enableJobRecovery"]=false; QVERIFY(Settings.Put(Section::Options,Options));
        { QMainOpts Dialog(main_); Dialog.LoadSettings(); Dialog.show(); QTest::qWait(100);
          auto* Nav=Dialog.findChild<QListWidget*>("lstOptCate"); QVERIFY(Nav); for(int I=0;I<Nav->count();++I) if(Nav->item(I)->text()==QStringLiteral("파일 작업")) Nav->setCurrentRow(I);
          auto* Tabs = Dialog.findChild<QTabWidget*>("hc.settings.fileOperationsTabs"); QVERIFY(Tabs); Tabs->setCurrentIndex(1);
          auto* Toggle=Dialog.findChild<QCheckBox*>("hc.settings.delayedFileWrites"); QVERIFY(Toggle && Toggle->isVisible() && Toggle->isChecked());
          QVERIFY(SaveR9Dialog(Dialog,"r23-file-writes-default-on")); Toggle->setChecked(false); QVERIFY(Dialog.SaveSettings());
          QVERIFY(SaveR9Dialog(Dialog,"r23-file-writes-forced-on")); Dialog.accept(); }
        QVERIFY(!Settings.CommittedObject()["options"].toObject()["delayedFileWrites"].toBool(true));
        { Repository Reload(QFileInfo(Settings.FileName()).absolutePath()); QVERIFY(!Reload.Read(Section::Options).toObject()["delayedFileWrites"].toBool(true)); }
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        const auto SourcePath=Directory.filePath("source.bin"); const QByteArray Payload(128*1024,'Q');
        { QFile Source(SourcePath); QVERIFY(Source.open(QIODevice::WriteOnly)); QCOMPARE(Source.write(Payload),qint64(Payload.size())); }
        auto Reader=MakeNativeReadProvider(LocatorKind::Local); auto Writer=MakeNativeWritableStorage(LocatorKind::Local,Reader);
        auto Source=Reader->Resolve({LocatorKind::Local,SourcePath.toStdString()},{}).get(); QVERIFY(Source.HasValue());
        QWidget Window; Window.setAttribute(Qt::WA_QuitOnClose,false); FileJobUi Jobs(&Window,std::make_shared<hc::commands::EventHub>());
        for(const bool Delayed:{false,true}) {
            Options["delayedFileWrites"]=Delayed; QVERIFY(Settings.Put(Section::Options,Options));
            const auto Target=Directory.filePath(Delayed?"delayed":"forced"); QVERIFY(QDir().mkpath(Target));
            FileOperationRequest Copy; Copy.operation_=hc::operations::FileOperation::Copy; Copy.sources_={{Source.Value(),Reader,Writer}};
            Copy.destination_={LocatorKind::Local,Target.toStdString()}; Copy.destinationWriter_=Writer;
            OperationMetrics::Instance().Reset(); auto Handle=Jobs.Submit(Copy,{}); QVERIFY(Handle.HasValue());
            QTRY_VERIFY_WITH_TIMEOUT(Handle.Value().completion_.wait_for(std::chrono::milliseconds(0))==std::future_status::ready,10000);
            QCOMPARE(Handle.Value().completion_.get().snapshot_.state_,JobState::Completed);
            QTRY_VERIFY(!Jobs.IsUiPending(Handle.Value().id_));
            QFile Result(QDir(Target).filePath("source.bin")); QVERIFY(Result.open(QIODevice::ReadOnly)); QCOMPARE(Result.readAll(),Payload);
            const auto Metrics=Directory.filePath(Delayed?"delayed.json":"forced.json"); QVERIFY(OperationMetrics::Instance().Save(Metrics,0));
            QFile MetricFile(Metrics); QVERIFY(MetricFile.open(QIODevice::ReadOnly));
            const auto Data=QJsonDocument::fromJson(MetricFile.readAll()).object()["metrics"].toObject();
            QCOMPARE(Data["payload.flush"].toObject()["count"].toInt(),Delayed?0:1);
            QCOMPARE(Data["payload.flush.skipped"].toObject()["count"].toInt(),Delayed?1:0);
        }
    }
    void todoBugOperationDetailsSettingUi()
    {
        using namespace hc::operations; using namespace hc::settings;
        auto& Settings=AppSettings(); const auto Original=Settings.Object();
        const auto Restore=qScopeGuard([&] { Settings.DiscardEdit(); Settings.Replace(Original); Settings.Commit(); });
        auto Options=Settings.Read(Section::Options).toObject(); Options.remove("showJobDetails"); Options["jobDetailed"]=false;
        Options["enableJobRecovery"]=false; Options["jobCloseOnSuccess"]=false; QVERIFY(Settings.Put(Section::Options,Options));
        { QMainOpts Dialog(main_); Dialog.LoadSettings(); auto* Check=Dialog.findChild<QCheckBox*>("hc.settings.showJobDetails");
          QVERIFY(Check && Check->isEnabled() && !Check->isChecked()); Check->setChecked(true); Dialog.reject(); }
        QVERIFY(!Settings.CommittedObject()["options"].toObject()["showJobDetails"].toBool());
        struct Control { std::atomic<bool> password_{false}; };
        class Task final : public IJobTask {
        public:
            explicit Task(std::shared_ptr<Control> Control):control_(std::move(Control)){}
            JobStep Advance(JobStepContext) override {
                const bool Password=control_->password_.load();
                JobStep Step{Password?JobState::WaitingForUser:JobState::Running};
                if(Password) { InteractionRequest Request; Request.id_="details-password"; Request.kind_=InteractionKind::Password; Request.choices_={InteractionChoice::Confirm,InteractionChoice::Skip}; Step.interaction_=Request; }
                else Step.deferFor_=std::chrono::milliseconds(20);
                return Step;
            }
            std::vector<hc::storage::Error> Finalize(JobState) override {return {};}
        private: std::shared_ptr<Control> control_;
        };
        QWidget Window; Window.setAttribute(Qt::WA_QuitOnClose,false); FileJobUi Jobs(&Window,std::make_shared<hc::commands::EventHub>());
        auto Control=std::make_shared<struct Control>(); PreparedJob Work; Work.request_.metadata_={"fixture","D:/source","D:/target","fixture-options"}; Work.task_=std::make_unique<Task>(Control);
        auto Started=Jobs.Submit(std::move(Work),QStringLiteral("상세 기록 설정 시험")); QVERIFY(Started.HasValue());
        auto* Dialog=Window.findChild<QDialog*>("hc.fileJobs"); auto* Details=Window.findChild<QPlainTextEdit*>("hc.jobs.details");
        auto* Toggle=Window.findChild<QPushButton*>("hc.jobs.toggleDetails"); auto* Notice=Window.findChild<QLabel*>("hc.jobs.notice");
        QVERIFY(Dialog && Details && Toggle && Notice); QTRY_VERIFY(Dialog->isVisible()); QVERIFY(!Details->isVisible());
        Toggle->click(); QTRY_VERIFY(Window.findChild<fm::ui::TransferGraph*>("hc.jobs.graph")->isVisible()); QVERIFY(!Details->isVisible());
        Options=Settings.Read(Section::Options).toObject(); Options["showJobDetails"]=true; QVERIFY(Settings.Put(Section::Options,Options)); Jobs.ApplyDefaults(); QTRY_VERIFY(Details->isVisible());
        Toggle->click(); QVERIFY(Details->isVisible());
        Options["showJobDetails"]=false; QVERIFY(Settings.Put(Section::Options,Options)); Jobs.ApplyDefaults(); QTRY_VERIFY(!Details->isVisible());
        Control->password_=true; QTRY_COMPARE(Jobs.Scheduler().Snapshot(Started.Value().id_).Value().state_,JobState::WaitingForUser);
        QTRY_VERIFY(Notice->isVisible()); QVERIFY(Notice->text().contains(QStringLiteral("암호"))); QVERIFY(!Details->isVisible());
        QTRY_COMPARE(Window.findChild<QLineEdit*>("hc.jobs.newName")->echoMode(),QLineEdit::Password);
        QVERIFY(SaveR9Dialog(*Dialog,"operation-progress-records-off-password")); Jobs.Scheduler().Cancel(Started.Value().id_);
        QTRY_VERIFY(Started.Value().completion_.wait_for(std::chrono::milliseconds(0))==std::future_status::ready);
        { QMainOpts Editor(main_); Editor.LoadSettings(); auto* Check=Editor.findChild<QCheckBox*>("hc.settings.showJobDetails"); Check->setChecked(true); QVERIFY(Editor.SaveSettings()); Editor.accept(); }
        Repository Reload(QFileInfo(Settings.FileName()).absolutePath()); QVERIFY(Reload.Read(Section::Options).toObject()["showJobDetails"].toBool());
    }
    void r22LaboratoryUi()
    {
        using namespace hc::settings; auto& Settings=AppSettings(); const auto Saved=Settings.CommittedObject()["options"].toObject(); Settings.BeginEdit();
        const auto Restore=qScopeGuard([&]{Settings.DiscardEdit();Settings.BeginEdit();Settings.Put(Section::Options,Saved);Settings.Commit(); TyStCommandMgr::GetInstance()->Bridge()->FileJobs()->ApplyDefaults();});
        auto Options=Settings.Read(Section::Options).toObject(); Options.remove("enableJobRecovery"); Options.remove("saveJobProgress"); QVERIFY(Settings.Put(Section::Options,Options));
        { QMainOpts Dialog(main_); Dialog.LoadSettings(); Dialog.show(); QTest::qWait(100);
          auto* Nav=Dialog.findChild<QListWidget*>("lstOptCate"); QVERIFY(Nav); for(int I=0;I<Nav->count();++I) if(Nav->item(I)->text()==QStringLiteral("실험실")) Nav->setCurrentRow(I);
          auto* Recovery=Dialog.findChild<QCheckBox*>("hc.settings.enableJobRecovery"); auto* Progress=Dialog.findChild<QCheckBox*>("hc.settings.saveJobProgress");
          QVERIFY(Recovery && Progress && Recovery->isVisible()); QVERIFY(!Recovery->isChecked()); QVERIFY(!Progress->isEnabled()); QVERIFY(SaveR9Dialog(Dialog,"r22-laboratory-default-off"));
          Recovery->setChecked(true); QVERIFY(Progress->isEnabled()); Progress->setChecked(true); Dialog.reject(); }
        QVERIFY(!Settings.Read(Section::Options).toObject()["enableJobRecovery"].toBool(false));
        { QMainOpts Dialog(main_); Dialog.LoadSettings(); Dialog.show(); QTest::qWait(100);
          auto* Nav=Dialog.findChild<QListWidget*>("lstOptCate"); for(int I=0;I<Nav->count();++I) if(Nav->item(I)->text()==QStringLiteral("실험실")) Nav->setCurrentRow(I);
          auto* Recovery=Dialog.findChild<QCheckBox*>("hc.settings.enableJobRecovery"); Recovery->setChecked(true); QVERIFY(Dialog.SaveSettings());
          QVERIFY(Settings.CommittedObject()["options"].toObject()["enableJobRecovery"].toBool()); QVERIFY(SaveR9Dialog(Dialog,"r22-laboratory-recovery-on")); Dialog.accept(); }
        { Repository Reload(QFileInfo(Settings.FileName()).absolutePath()); QVERIFY(Reload.Read(Section::Options).toObject()["enableJobRecovery"].toBool()); }
        { QMainOpts Dialog(main_); Dialog.LoadSettings(); QVERIFY(Dialog.findChild<QCheckBox*>("hc.settings.enableJobRecovery")->isChecked());
          Dialog.findChild<QCheckBox*>("hc.settings.enableJobRecovery")->setChecked(false); QVERIFY(!Dialog.findChild<QCheckBox*>("hc.settings.saveJobProgress")->isEnabled()); QVERIFY(Dialog.SaveSettings()); Dialog.accept(); }
        QVERIFY(!Settings.CommittedObject()["options"].toObject()["enableJobRecovery"].toBool(true));
    }
    void r20LaboratoryUi()
    {
        using namespace hc::settings; auto& Settings=AppSettings(); const auto Original=Settings.Object();
        const auto Restore=qScopeGuard([&]{Settings.DiscardEdit();Settings.Replace(Original);Settings.Commit();TyStCommandMgr::GetInstance()->Bridge()->FileJobs()->ApplyDefaults();});
        auto Options=Settings.Read(Section::Options).toObject(); Options.remove("saveJobProgress"); Options["enableJobRecovery"]=true; QVERIFY(Settings.Put(Section::Options,Options));
        { QMainOpts Dialog(main_); Dialog.show(); Dialog.LoadSettings();
          auto* Categories=Dialog.findChild<QListWidget*>("lstOptCate"); QVERIFY(Categories); bool Found=false;
          for(int I=0;I<Categories->count();++I)if(Categories->item(I)->text()==QStringLiteral("실험실")){Categories->setCurrentRow(I);Found=true;} QVERIFY(Found);
          auto* Check=Dialog.findChild<QCheckBox*>("hc.settings.saveJobProgress"); QVERIFY(Check && Check->isVisible() && Check->isEnabled()); QVERIFY(!Check->isChecked());
          QVERIFY(SaveR9Dialog(Dialog,"r20-laboratory-default")); Check->setChecked(true); Dialog.reject(); }
        QVERIFY(!Settings.Read(Section::Options).toObject()["saveJobProgress"].toBool(false));
        { QMainOpts Dialog(main_); Dialog.show(); Dialog.LoadSettings(); auto* Categories=Dialog.findChild<QListWidget*>("lstOptCate");
          for(int I=0;I<Categories->count();++I)if(Categories->item(I)->text()==QStringLiteral("실험실"))Categories->setCurrentRow(I);
          auto* Check=Dialog.findChild<QCheckBox*>("hc.settings.saveJobProgress"); Check->setChecked(true); QVERIFY(Dialog.SaveSettings());
          QVERIFY(Settings.CommittedObject()["options"].toObject()["saveJobProgress"].toBool()); QVERIFY(SaveR9Dialog(Dialog,"r20-laboratory-enabled"));
          Check->setChecked(false); Dialog.reject(); }
        { Repository Reload(QFileInfo(Settings.FileName()).absolutePath()); QVERIFY(Reload.Read(Section::Options).toObject()["saveJobProgress"].toBool()); }
        { QMainOpts Dialog(main_); Dialog.LoadSettings(); QVERIFY(Dialog.findChild<QCheckBox*>("hc.settings.saveJobProgress")->isChecked()); Dialog.reject(); }
    }
    void r13SettingsOptionsUi()
    {
        using namespace hc::settings; using namespace hc::operations; using namespace hc::storage;
        auto& Settings = AppSettings(); const auto Original = Settings.Object(); const auto Workspace = main_->CaptureWorkspace();
        const auto Restore = qScopeGuard([&] { Settings.DiscardEdit(); Settings.Replace(Original); Settings.Commit(); main_->RestoreWorkspace(Workspace); main_->SaveWorkspace(); });
        auto* Left = main_->findChild<CmpPanel*>("cmpLeftPanel"); QVERIFY(Left);
        auto* View = static_cast<CGridView*>(Left->GetFocusView(Left->CurrentTabIndex()));
        { QMainOpts Dialog(main_); Dialog.SetListView(View); Dialog.show(); Dialog.LoadSettings(); QTest::qWait(30);
          auto* Single = Dialog.findChild<QCheckBox*>("hc.settings.singleInstance"); QVERIFY(Single && Single->isEnabled()); Single->setChecked(true);
          Dialog.findChild<QComboBox*>("hc.settings.newTabPosition")->setCurrentIndex(1);
          Dialog.findChild<QCheckBox*>("hc.settings.recycleDefault")->setChecked(false);
          Dialog.findChild<QComboBox*>("hc.settings.jobNotification")->setCurrentIndex(0);
          Dialog.findChild<QComboBox*>("hc.settings.queueOrder")->setCurrentIndex(1);
          Dialog.findChild<QSpinBox*>("hc.settings.jobConcurrency")->setValue(1);
          const auto Output = qEnvironmentVariable("HC_UI_ARTIFACT_DIR");
          if (!Output.isEmpty()) {
              QDir().mkpath(Output); auto* Categories = Dialog.findChild<QListWidget*>("lstOptCate");
              for (const auto& Page : {QString("일반 · 모양"), QString("파일 작업")}) {
                  for (int Index = 0; Index < Categories->count(); ++Index) if (Categories->item(Index)->text() == Page) Categories->setCurrentRow(Index);
                  QTest::qWait(100); const auto Name = Page == "파일 작업" ? QString("r13-job-defaults") : QString("r13-general-tabs");
                  QVERIFY(Dialog.grab().save(QDir(Output).filePath(Name + ".png")));
                  auto& Theme = fm::style::ThemeManager::instance(); const auto BeforeTheme = Theme.scheme(); Theme.setScheme(fm::style::ThemeManager::Scheme::Dark); QTest::qWait(50);
                  QVERIFY(Dialog.grab().save(QDir(Output).filePath(Name + "-dark.png"))); Theme.setScheme(BeforeTheme);
                  if (Page == "파일 작업") {
                      auto* Tabs = Dialog.findChild<QTabWidget*>("hc.settings.fileOperationsTabs"); QVERIFY(Tabs);
                      for (int Tab = 0; Tab < Tabs->count(); ++Tab) {
                          Tabs->setCurrentIndex(Tab);
                          for (const bool Dark : {false, true}) {
                              Theme.setScheme(Dark ? fm::style::ThemeManager::Scheme::Dark : fm::style::ThemeManager::Scheme::Light); QTest::qWait(50);
                              QVERIFY(Dialog.grab().save(QDir(Output).filePath(QString("file-operations-%1-%2.png").arg(Tab).arg(Dark ? "dark" : "light"))));
                              auto* Scroll = qobject_cast<QScrollArea*>(Tabs->currentWidget()); QVERIFY(Scroll);
                              Scroll->verticalScrollBar()->setValue(Scroll->verticalScrollBar()->maximum()); QTest::qWait(30);
                              QVERIFY(Dialog.grab().save(QDir(Output).filePath(QString("file-operations-%1-%2-bottom.png").arg(Tab).arg(Dark ? "dark" : "light"))));
                              Scroll->verticalScrollBar()->setValue(0);
                          }
                      }
                      Tabs->setCurrentIndex(0); Theme.setScheme(BeforeTheme);
                  }
              }
          }
          QVERIFY(Dialog.SaveSettings()); QCOMPARE(TyStCommandMgr::GetInstance()->Bridge()->FileJobs()->Scheduler().ConcurrencyLimit(), size_t(1));
          const auto Saved = Settings.CommittedObject(); Dialog.findChild<QSpinBox*>("hc.settings.jobConcurrency")->setValue(2); Dialog.reject(); QCOMPARE(Settings.Object(), Saved); }
        QCOMPARE(Settings.Read(Section::General).toObject()["singleInstance"].toBool(), true);
        QTemporaryDir Directory; QVERIFY(Directory.isValid()); QVERIFY(QDir().mkdir(Directory.filePath("target")));
        hc::workspace::PaneState Pane;
        for (int Index = 0; Index < 3; ++Index) { hc::workspace::TabState Tab; Tab.id_ = QString("r13-tab-%1").arg(Index); Tab.location_ = {LocatorKind::Local, Directory.path().toStdString()}; Pane.tabs_.append(Tab); }
        Pane.active_ = 1; Left->RestoreWorkspace(Pane); QTRY_VERIFY(!model(Left)->IsLoading());
        // Restore appends tabs in saved order regardless of the user's insertion policy.
        QCOMPARE(Left->CaptureWorkspace().tabs_[2].id_, QString("r13-tab-2"));
        for (const auto& Position : {QString("right"), QString("left"), QString("first"), QString("last")}) {
            Left->RestoreWorkspace(Pane); auto General = Settings.Read(Section::General).toObject(); General["newTabPosition"] = Position; QVERIFY(Settings.Put(Section::General, General));
            Left->AddTab(); const int Expected = Position == "right" ? 2 : Position == "left" ? 1 : Position == "first" ? 0 : 3;
            QCOMPARE(Left->CurrentTabIndex(), Expected); const auto Added = Left->CaptureWorkspace(); QCOMPARE(Added.tabs_.size(), 4);
            QStringList Originals; for (const auto& Tab : Added.tabs_) if (Tab.id_.startsWith("r13-tab-")) Originals.append(Tab.id_); QCOMPARE(Originals, QStringList({"r13-tab-0", "r13-tab-1", "r13-tab-2"}));
        }
        hc::ui::FileOperationDialogRequest Request; Request.command_ = "hc.file.delete"; Request.source_ = Directory.path(); Request.nativeSource_ = true; Request.names_ = {"sample.txt"};
        { hc::ui::FileOperationDialog Dialog(Request, main_); QVERIFY(Dialog.findChild<QRadioButton*>("hc.operation.permanent")->isChecked()); QVERIFY(!Dialog.Recycle()); }
        auto Options = Settings.Read(Section::Options).toObject(); Options["deleteToRecycle"] = true; QVERIFY(Settings.Put(Section::Options, Options));
        Request.source_ = "\\\\r13-unavailable-server\\share";
        { hc::ui::FileOperationDialog Dialog(Request, main_); QTRY_VERIFY(!Dialog.findChild<QRadioButton*>("hc.operation.recycle")->isEnabled()); QVERIFY(!Dialog.findChild<QRadioButton*>("hc.operation.permanent")->isChecked()); QVERIFY(!Dialog.AcceptButton()->isEnabled()); }
        QMainWindow Window; Window.setAttribute(Qt::WA_QuitOnClose, false); FileJobUi Jobs(&Window, std::make_shared<hc::commands::EventHub>());
        QCOMPARE(Jobs.Scheduler().ConcurrencyLimit(), size_t(1));
        auto Reader = MakeNativeReadProvider(LocatorKind::Local); auto Writer = MakeNativeWritableStorage(LocatorKind::Local, Reader); std::vector<JobHandle> Handles;
        for (int Index = 0; Index < 3; ++Index) {
            const auto Path = Directory.filePath(QString("queue-%1.txt").arg(Index)); { QFile File(Path); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("R13 queue"); }
            const auto Entry = Reader->Resolve({LocatorKind::Local, Path.toStdString()}, {}).get(); QVERIFY(Entry.HasValue());
            FileOperationRequest Copy; Copy.sources_ = {{Entry.Value(), Reader, Writer}}; Copy.destination_ = {LocatorKind::Local, Directory.filePath("target").toStdString()}; Copy.destinationWriter_ = Writer;
            const auto Handle = Jobs.Submit(Copy, {}, true); QVERIFY(Handle.HasValue()); Handles.push_back(Handle.Value());
        }
        QVERIFY(Jobs.Scheduler().Snapshot(Handles[2].id_).Value().queueOrder_ < Jobs.Scheduler().Snapshot(Handles[0].id_).Value().queueOrder_);
        Settings.BeginEdit(); Options["jobConcurrency"] = 2; QVERIFY(Settings.Put(Section::Options, Options)); Jobs.ApplyDefaults(); QCOMPARE(Jobs.Scheduler().ConcurrencyLimit(), size_t(1)); Settings.DiscardEdit();
        Jobs.Scheduler().SetConcurrencyLimit(2); Jobs.ApplyDefaults(); QCOMPARE(Jobs.Scheduler().ConcurrencyLimit(), size_t(2)); // Preserve a session override.
        Options["jobConcurrency"] = 2; QVERIFY(Settings.Put(Section::Options, Options)); Jobs.ApplyDefaults(); QCOMPARE(Jobs.Scheduler().ConcurrencyLimit(), size_t(2));
        Options["jobConcurrency"] = 1; QVERIFY(Settings.Put(Section::Options, Options)); Jobs.ApplyDefaults(); QCOMPARE(Jobs.Scheduler().ConcurrencyLimit(), size_t(1));
        Jobs.Scheduler().Control({Handles[0].id_, Handles[1].id_, Handles[2].id_}, JobControl::Start);
        for (const auto& Handle : Handles) { QTRY_VERIFY(Handle.completion_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready); QTRY_VERIFY(!Jobs.IsUiPending(Handle.id_)); }
        QVERIFY(!Window.property("hc.jobs.lastNotification").isValid());
        // Capture of notification policy is independent of settings changed after submission.
        Options["jobNotification"] = "status"; QVERIFY(Settings.Put(Section::Options, Options));
        { QFile File(Directory.filePath("notify.txt")); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("notify"); }
        auto Entry = Reader->Resolve({LocatorKind::Local, Directory.filePath("notify.txt").toStdString()}, {}).get(); QVERIFY(Entry.HasValue());
        FileOperationRequest Copy; Copy.sources_ = {{Entry.Value(), Reader, Writer}}; Copy.destination_ = {LocatorKind::Local, Directory.filePath("target").toStdString()}; Copy.destinationWriter_ = Writer;
        auto Handle = Jobs.Submit(Copy, {}, true); QVERIFY(Handle.HasValue()); Options["jobNotification"] = "none"; QVERIFY(Settings.Put(Section::Options, Options));
        Jobs.Scheduler().Control({Handle.Value().id_}, JobControl::Start); QTRY_VERIFY(!Jobs.IsUiPending(Handle.Value().id_)); QVERIFY(!Window.property("hc.jobs.lastNotification").toString().isEmpty());
        Repository Reload(QFileInfo(Settings.FileName()).absolutePath()); QCOMPARE(Reload.Object(), Settings.Object());
        QProcess Restart; auto Environment = QProcessEnvironment::systemEnvironment(); Environment.insert("HC_R13_RESTART", "1"); Environment.insert("HC_TEST_CONFIG_DIR", QFileInfo(Settings.FileName()).absolutePath()); Restart.setProcessEnvironment(Environment);
        Restart.start(QCoreApplication::applicationFilePath(), {"r13RestartUi", "-o", "r13-restart.txt,txt"}); QVERIFY(Restart.waitForStarted(5000)); QTRY_VERIFY_WITH_TIMEOUT(Restart.state() == QProcess::NotRunning, 30000); QCOMPARE(Restart.exitCode(), 0);
    }
    void r13RestartUi()
    {
        if (qEnvironmentVariableIsSet("HC_R13_AUTO_RESTART")) {
            auto* Left = main_->findChild<CmpPanel*>("cmpLeftPanel"); QTRY_VERIFY(!model(Left)->IsLoading());
            auto* View = static_cast<CGridView*>(Left->GetFocusView(Left->CurrentTabIndex()));
            const bool Manual = qEnvironmentVariableIsSet("HC_R13_AUTO_MANUAL"); QCOMPARE(View->AutomaticColumns(), !Manual);
            QTRY_COMPARE_WITH_TIMEOUT(View->ColumnProfile().id_, Manual ? hc::columns::ReferenceTwoLineProfileId : hc::columns::ReferenceProfileId, 10000);
            return;
        }
        if (!qEnvironmentVariableIsSet("HC_R13_RESTART")) QSKIP("R13 restart child only");
        const auto General = hc::settings::AppSettings().Read(hc::settings::Section::General).toObject(); QVERIFY(General["singleInstance"].toBool()); QCOMPARE(General["newTabPosition"].toString(), QString("last"));
        QCOMPARE(TyStCommandMgr::GetInstance()->Bridge()->FileJobs()->Scheduler().ConcurrencyLimit(), size_t(1));
        QCOMPARE(hc::settings::AppSettings().Read(hc::settings::Section::Options).toObject()["queueInsertion"].toString(), QString("first"));
    }
    void r13SingleInstanceUi()
    {
        using hc::settings::SingleInstance; QTemporaryDir Directory; QTemporaryDir Other; QVERIFY(Directory.isValid() && Other.isValid());
        SingleInstance Owner; QJsonArray Received; bool Accept = true;
        QCOMPARE(Owner.Start(Directory.path(), {}, [&](const QJsonArray& Paths) { Received = Paths; return Accept; }), SingleInstance::Result::Primary);
        const auto Probe = [&](const QString& Config, const QStringList& Args, int Expected) {
            QProcess Child; QStringList Arguments{"--r13-ipc-probe", Config}; Arguments.append(Args); Child.start(QCoreApplication::applicationFilePath(), Arguments); QVERIFY(Child.waitForStarted(5000)); QTRY_VERIFY_WITH_TIMEOUT(Child.state() == QProcess::NotRunning, 15000); QCOMPARE(Child.exitCode(), Expected);
        };
        Probe(Directory.path(), {"--left", Directory.path(), "--right=" + Other.path()}, 0); QCOMPARE(Received.size(), 2); QCOMPARE(Received[0].toObject()["side"].toString(), QString("left")); QCOMPARE(Received[1].toObject()["path"].toString(), Other.path());
        Probe(Other.path(), {}, 2); Probe(Directory.path(), {}, 0); Accept = false; Probe(Directory.path(), {Directory.path()}, 3); Accept = true;
        // A crashed predecessor leaves no permanent ownership lock.
        Probe(Other.path(), {}, 2); SingleInstance Recovery; QCOMPARE(Recovery.Start(Other.path(), {}, [](const QJsonArray&) { return true; }), SingleInstance::Result::Primary);
        const auto Original = main_->CaptureWorkspace(); const auto Restore = qScopeGuard([&] { main_->RestoreWorkspace(Original); });
        auto* Left = main_->findChild<CmpPanel*>("cmpLeftPanel"); auto* Right = main_->findChild<CmpPanel*>("cmpRightPanel");
        const int LeftCount = Left->CaptureWorkspace().tabs_.size(), RightCount = Right->CaptureWorkspace().tabs_.size();
        const int ActivePane = main_->CaptureWorkspace().activePane_;
        QVERIFY(main_->OpenStartupPaths(QJsonArray{QJsonObject{{"side", "left"}, {"path", Directory.path()}}, QJsonObject{{"side", "right"}, {"path", Other.path()}}, QJsonObject{{"side", "active"}, {"path", Directory.path()}}}));
        QCOMPARE(Left->CaptureWorkspace().tabs_.size(), LeftCount + 1 + (ActivePane == 0)); QCOMPARE(Right->CaptureWorkspace().tabs_.size(), RightCount + 1 + (ActivePane == 1));
        QTRY_VERIFY(!model(Left)->IsLoading() && !model(Right)->IsLoading());
        QCOMPARE(QDir::cleanPath(Left->CurrentPath()), Directory.path()); QCOMPARE(QDir::cleanPath(Right->CurrentPath()), ActivePane == 1 ? Directory.path() : Other.path());
    }
    void r13AutomaticColumnsUi()
    {
        using namespace hc::columns; using namespace hc::settings; using namespace hc::storage;
        auto& Settings = AppSettings(); const auto Original = Settings.Object(); const auto Workspace = main_->CaptureWorkspace();
        const auto Restore = qScopeGuard([&] { Settings.DiscardEdit(); Settings.Replace(Original); Settings.Commit(); main_->RestoreWorkspace(Workspace); main_->SaveWorkspace(); });
        QTemporaryDir Directory; QVERIFY(Directory.isValid()); QVERIFY(QDir().mkdir(Directory.filePath("other")));
        for (const auto& Name : {"a.txt", "b.txt", "c.bin"}) { QFile File(Directory.filePath(Name)); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("R13 columns"); }
        auto Colors = FileColorRules::Empty(); Colors["groups"] = QJsonArray{QJsonObject{{"id", "r13-text"}, {"name", "텍스트"}, {"enabled", true}, {"join", "any"}, {"conditions", QJsonArray{QJsonObject{{"type", "extension"}, {"value", "txt"}}}}}};
        auto Options = Settings.Read(Section::Options).toObject(); Options["fileColorRules"] = Colors; QVERIFY(Settings.Put(Section::Options, Options));
        QJsonObject Library; for (const auto& Profile : {DefaultProfile(), ReferenceProfile(), ReferenceProfile(true)}) Library[IdText(Profile.id_)] = EncodeProfile(Profile);
        auto Policy = EmptyAutoSelection(); Policy["enabled"] = true;
        const QJsonObject Rule{{"enabled", true}, {"profile", IdText(ReferenceProfileId)}, {"path", Directory.path()}, {"group", "r13-text"}, {"percent", 60}};
        Policy["rules"] = QJsonArray{Rule}; QVERIFY(ValidateAutoSelection(Policy, Library, Colors).isEmpty());
        auto Columns = Settings.Read(Section::Columns).toObject(); Columns["ColumnsV2/Profiles"] = Library; Columns[AutoSelectionKey] = Policy; QVERIFY(Settings.Put(Section::Columns, Columns));
        auto* Left = main_->findChild<CmpPanel*>("cmpLeftPanel"); hc::workspace::TabState Tab; Tab.id_ = "r13-auto"; Tab.location_ = {LocatorKind::Local, Directory.path().toStdString()}; hc::workspace::PaneState Pane; Pane.tabs_ = {Tab}; Left->RestoreWorkspace(Pane);
        QTRY_VERIFY(!model(Left)->IsLoading()); auto* View = static_cast<CGridView*>(Left->GetFocusView(0)); View->SetAutomaticColumns(true);
        QTRY_COMPARE_WITH_TIMEOUT(View->ColumnProfile().id_, ReferenceProfileId, 10000);
        const auto RestartColumns = [&](bool Manual) {
            QVERIFY(main_->SaveWorkspace()); QProcess Child; auto Environment = QProcessEnvironment::systemEnvironment();
            Environment.insert("HC_R13_AUTO_RESTART", "1"); if (Manual) Environment.insert("HC_R13_AUTO_MANUAL", "1"); Environment.insert("HC_TEST_CONFIG_DIR", QFileInfo(Settings.FileName()).absolutePath()); Child.setProcessEnvironment(Environment);
            Child.start(QCoreApplication::applicationFilePath(), {"r13RestartUi", "-o", Manual ? "r13-manual-restart.txt,txt" : "r13-auto-restart.txt,txt"}); QVERIFY(Child.waitForStarted(5000)); QTRY_VERIFY_WITH_TIMEOUT(Child.state() == QProcess::NotRunning, 30000); QCOMPARE(Child.exitCode(), 0);
        };
        RestartColumns(false);
        { QMainOpts Dialog(main_); Dialog.SetListView(View); Dialog.show(); Dialog.LoadSettings(); Dialog.ShowListSettingsPage();
          const auto Output = qEnvironmentVariable("HC_UI_ARTIFACT_DIR"); if (!Output.isEmpty()) {
              QTest::qWait(100); QVERIFY(Dialog.grab().save(QDir(Output).filePath("r13-auto-columns.png")));
              auto& Theme = fm::style::ThemeManager::instance(); const auto BeforeTheme = Theme.scheme(); Theme.setScheme(fm::style::ThemeManager::Scheme::Dark); QTest::qWait(50); QVERIFY(Dialog.grab().save(QDir(Output).filePath("r13-auto-columns-dark.png"))); Theme.setScheme(BeforeTheme);
              const auto BeforeFont = Dialog.font(); auto Large = BeforeFont; Large.setPointSize(18); Dialog.setFont(Large); QTest::qWait(50); QVERIFY(Dialog.grab().save(QDir(Output).filePath("r13-auto-columns-large.png"))); Dialog.setFont(BeforeFont);
          }
          auto* Editor = Dialog.findChild<QtColumnProfileEditor*>("hc.columns.editor"); QVERIFY(Editor);
          auto* RulesTable = Editor->findChild<QTableWidget*>("hc.columns.autoRules"); QVERIFY(RulesTable); QCOMPARE(qobject_cast<QComboBox*>(RulesTable->cellWidget(0, 3))->currentText(), QString("텍스트"));
          auto* Profiles = Editor->findChild<QListWidget*>("hc.columns.profileList");
          for (int Index = 0; Index < Profiles->count(); ++Index) if (Profiles->item(Index)->data(Qt::UserRole).toString() == IdText(ReferenceTwoLineProfileId)) Profiles->setCurrentRow(Index);
          QVERIFY(!Editor->AutomaticTab()); Dialog.reject(); }
        QVERIFY(View->AutomaticColumns());
        // Search/page visibility does not alter the folder's denominator.
        Left->SetSearch("c.bin"); QTest::qWait(150); QCOMPARE(View->ColumnProfile().id_, ReferenceProfileId); Left->SetSearch({});
        QVERIFY(View->ApplyColumnProfile(ReferenceProfile(true))); QVERIFY(!View->AutomaticColumns());
        auto Changed = Rule; Changed["profile"] = IdText(BuiltInProfileId); Policy["rules"] = QJsonArray{Changed}; Columns[AutoSelectionKey] = Policy; QVERIFY(Settings.Put(Section::Columns, Columns)); View->ApplyDisplaySettings(); QTest::qWait(200); QCOMPARE(View->ColumnProfile().id_, ReferenceTwoLineProfileId);
        const auto Manual = View->CaptureWorkspaceView(); View->RestoreWorkspaceView(Manual); QVERIFY(!View->AutomaticColumns()); RestartColumns(true);
        View->SetAutomaticColumns(true); QTRY_COMPARE(View->ColumnProfile().id_, BuiltInProfileId);
        Left->ChangeCurrentPath(Directory.filePath("other"), {}); QTRY_VERIFY(!model(Left)->IsLoading()); QTRY_COMPARE(View->ColumnProfile().id_, ReferenceTwoLineProfileId); // Restore baseline when no rule matches.
        { QMainOpts Dialog(main_); Dialog.SetListView(View); Dialog.show(); Dialog.LoadSettings(); auto* Editor = Dialog.findChild<QtColumnProfileEditor*>("hc.columns.editor"); QVERIFY(Editor);
          Editor->findChild<QCheckBox*>("hc.columns.autoEnabled")->setChecked(false); QVERIFY(Dialog.SaveSettings()); QVERIFY(!Settings.CommittedObject()["columns"].toObject()[AutoSelectionKey].toObject()["enabled"].toBool()); Dialog.reject(); }
        // Whole index, not the first 50 samples or one 256-item page.
        class Entries final : public IEntryIndex {
        public:
            std::uint64_t Count() const override { return 600; }
            Result<EntryPage> Read(std::uint64_t Offset, std::size_t Max, const OperationContext&) const override {
                EntryPage Page; for (auto I = Offset; I < std::min<std::uint64_t>(600, Offset + Max); ++I) { EntrySnapshot E; E.name_ = I < 400 ? "data.txt" : "data.bin"; Page.entries_.push_back(E); } return Page;
            }
        };
        Policy["rules"] = QJsonArray{Rule}; Policy["enabled"] = true;
        const auto Matched = SelectAutomaticColumns(Policy, Library, Colors, Directory.path(), std::make_shared<Entries>()); QVERIFY(Matched.HasValue() && Matched.Value()); QCOMPARE(Matched.Value()->id_, ReferenceProfileId);
        auto V2Colors = hc::settings::FileColorRules::Upgrade(Colors);
        const auto SetV2Condition = [&](const QJsonObject& Condition) { auto Groups = V2Colors["groups"].toArray(); auto G = Groups[0].toObject(); G["conditions"] = QJsonArray{Condition}; Groups[0] = G; V2Colors["groups"] = Groups; };
        SetV2Condition({{"type", "pattern"}, {"op", "matches"}, {"value", QJsonArray{"data.txt", "*.json"}}});
        const auto V2Match = SelectAutomaticColumns(Policy, Library, V2Colors, Directory.path(), std::make_shared<Entries>()); QVERIFY(V2Match.HasValue() && V2Match.Value());
        auto V3Colors = V2Colors; V3Colors["version"] = 3;
        const auto SetV3Condition = [&](const QString& Syntax, const QString& Op, const QJsonValue& Value) {
            auto Groups = V3Colors["groups"].toArray(); auto G = Groups[0].toObject();
            G["conditions"] = QJsonArray{QJsonObject{{"type", "pattern"}, {"syntax", Syntax}, {"op", Op}, {"value", Value}}}; Groups[0] = G; V3Colors["groups"] = Groups;
        };
        SetV3Condition("regex", "matches", "\\Adata\\.txt\\z");
        const auto V3Match = SelectAutomaticColumns(Policy, Library, V3Colors, Directory.path(), std::make_shared<Entries>()); QVERIFY(V3Match.HasValue() && V3Match.Value());
        SetV3Condition("wildcard", "matches", QJsonArray{"data.t?t"});
        const auto V3Glob = SelectAutomaticColumns(Policy, Library, V3Colors, Directory.path(), std::make_shared<Entries>()); QVERIFY(V3Glob.HasValue() && V3Glob.Value());
        SetV3Condition("regex", "matches", "(");
        QVERIFY(!SelectAutomaticColumns(Policy, Library, V3Colors, Directory.path(), std::make_shared<Entries>()).HasValue());
        class PathologicalEntries final : public IEntryIndex {
        public:
            std::uint64_t Count() const override { return 1; }
            Result<EntryPage> Read(std::uint64_t, std::size_t, const OperationContext&) const override { EntryPage Page; EntrySnapshot E; E.name_ = std::string(500, 'a') + '!'; Page.entries_.push_back(E); return Page; }
        };
        SetV3Condition("regex", "notMatches", "(a+)+$");
        QVERIFY(!SelectAutomaticColumns(Policy, Library, V3Colors, Directory.path(), std::make_shared<PathologicalEntries>()).HasValue());
        class MetadataEntries final : public IEntryIndex {
        public:
            bool missing_ = false, links_ = false;
            std::uint64_t Count() const override { return 600; }
            Result<EntryPage> Read(std::uint64_t Offset, std::size_t Max, const OperationContext&) const override {
                EntryPage Page; for (auto I = Offset; I < std::min<std::uint64_t>(600, Offset + Max); ++I) { EntrySnapshot E; E.name_ = "data.bin";
                    E.modifiedUtcMs_ = I < 400 && !missing_ ? QDateTime::currentMSecsSinceEpoch() - 3600000 : -11644473600000LL;
                    if (links_) { E.attributes_ = 0x400; E.hints_["reparseTag"] = I < 400 ? "2684354572" : "0"; }
                    Page.entries_.push_back(E); } return Page;
            }
        };
        auto Metadata = std::make_shared<MetadataEntries>();
        SetV2Condition({{"type", "modified"}, {"op", "within"}, {"value", "24"}, {"unit", "hours"}});
        const auto DateMatch = SelectAutomaticColumns(Policy, Library, V2Colors, Directory.path(), Metadata); QVERIFY(DateMatch.HasValue() && DateMatch.Value());
        Metadata->missing_ = true; const auto UnknownDates = SelectAutomaticColumns(Policy, Library, V2Colors, Directory.path(), Metadata); QVERIFY(UnknownDates.HasValue() && !UnknownDates.Value());
        Metadata->links_ = true; SetV2Condition({{"type", "link"}, {"op", "is"}, {"value", "symbolic"}});
        const auto LinkMatch = SelectAutomaticColumns(Policy, Library, V2Colors, Directory.path(), Metadata); QVERIFY(LinkMatch.HasValue() && LinkMatch.Value());
        Changed = Rule; Changed["percent"] = 70; Policy["rules"] = QJsonArray{Changed}; const auto Miss = SelectAutomaticColumns(Policy, Library, Colors, Directory.path(), std::make_shared<Entries>()); QVERIFY(Miss.HasValue() && !Miss.Value());
        std::stop_source Stop; Stop.request_stop(); OperationContext Context; Context.stopToken_ = Stop.get_token(); QVERIFY(!SelectAutomaticColumns(Policy, Library, Colors, Directory.path(), std::make_shared<Entries>(), Context).HasValue());
        Changed["group"] = "deleted"; Policy["rules"] = QJsonArray{Changed}; QVERIFY(!ValidateAutoSelection(Policy, Library, Colors).isEmpty());
        Policy["enabled"] = false; QVERIFY(ValidateAutoSelection(Policy, Library, Colors).isEmpty());
    }

    void r12ReferenceMainUi()
    {
        using namespace hc::columns;
        auto& Settings = hc::settings::AppSettings(); const auto Original = Settings.Object();
        const auto Workspace = main_->CaptureWorkspace(); const auto Size = main_->size();
        auto* Colors = TyStColorSchemeMgr::GetInstance(); const auto Scheme = Colors->GetColorScheme(Colors->GetCurrentColorScheme());
        auto* Bridge = TyStCommandMgr::GetInstance()->Bridge();
        const auto Restore = qScopeGuard([&] {
            Settings.DiscardEdit(); Settings.Replace(Original); Settings.Commit();
            emit Colors->sigColorSchemeChanged(Scheme); TyStCommandMgr::GetInstance()->Refresh(); Bridge->RefreshFunctionBar();
            main_->showNormal(); main_->resize(Size); main_->RestoreWorkspace(Workspace); main_->SaveWorkspace();
        });
        QTemporaryDir Directory; QVERIFY(Directory.isValid()); QVERIFY(QDir(Directory.path()).mkdir("자료 폴더"));
        for (const auto& Name : {QString("report.final.txt"), QString("README"), QString(".gitignore"), QString("긴 한글 파일 이름을 확인하는 자료.tar.gz")}) {
            QFile File(Directory.filePath(Name)); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("R12 fixture");
        }
        auto* Left = main_->findChild<CmpPanel*>("cmpLeftPanel"); auto* Right = main_->findChild<CmpPanel*>("cmpRightPanel");
        hc::workspace::TabState Tab; Tab.id_ = "r12-reference"; Tab.location_ = {hc::storage::LocatorKind::Local, Directory.path().toStdString()};
        hc::workspace::PaneState Pane; Pane.tabs_ = {Tab}; Left->RestoreWorkspace(Pane); Pane.tabs_[0].id_ += "-right"; Right->RestoreWorkspace(Pane);
        QTRY_VERIFY(!model(Left)->IsLoading() && !model(Right)->IsLoading());
        auto* View = static_cast<CGridView*>(Left->GetFocusView(0)); auto* Other = static_cast<CGridView*>(Right->GetFocusView(0));
        auto* Model = static_cast<CFSModel*>(model(Left));
        const auto RowOf = [&](const QString& Name) { for (int Row = 0; Row < Model->rowCount(); ++Row) if (Model->GetName(Model->index(Row, 0)) == Name) return Row; return -1; };
        const int Row = RowOf("report.final.txt"); QVERIFY(Row >= 0);
        const auto Key = hc::workspace::SelectionKey(*Model->GetFileInfo(Model->index(Row, 0))->storageEntry_); View->RestoreSelection({Key}, Key);
        const auto Capture = [&](const QString& Name) {
            const auto Out = qEnvironmentVariable("HC_UI_ARTIFACT_DIR"); if (Out.isEmpty()) return true;
            QDir().mkpath(Out); QTest::qWait(120); return main_->grab().save(QDir(Out).filePath(Name + '-' + QString::number(main_->devicePixelRatioF()) + ".png"));
        };
        main_->showNormal(); main_->resize(1420, 900); Left->sig_NotifyPanelActivated();
        auto Light = Colors->GetColorScheme(QStringLiteral("기본 라이트")); Light.FileList_Font.setPixelSize(13);
        emit Colors->sigColorSchemeChanged(Light);
        QVERIFY(View->ApplyColumnProfile(DefaultProfile())); QVERIFY(Other->ApplyColumnProfile(DefaultProfile()));
        QVERIFY(Capture("r12-existing-profile"));
        { QtColumnProfileEditor Editor; auto Saved = ReferenceProfile(); Saved.title_ = "사용자 보존";
          const auto Future = QJsonObject{{"version", 99}, {"future", "preserve"}};
          Editor.Load(DefaultProfile(), {{IdText(Saved.id_), EncodeProfile(Saved)}, {"future", Future}});
          QCOMPARE(Editor.Profile().id_, BuiltInProfileId); QVERIFY(!Editor.Dirty());
          QCOMPARE(Editor.Library()[IdText(Saved.id_)].toObject(), EncodeProfile(Saved)); QCOMPARE(Editor.Library()["future"].toObject(), Future);
          QVERIFY(Editor.Library().contains(IdText(ReferenceTwoLineProfileId))); }
        { QMainOpts Dialog(main_); Dialog.SetListView(View); Dialog.ShowListSettingsPage(); Dialog.show(); QTest::qWait(80); Dialog.LoadSettings();
          auto* Editor = Dialog.findChild<QtColumnProfileEditor*>(); QVERIFY(Editor);
          auto* Profiles = Editor->findChild<QListWidget*>("hc.columns.profileList"); QVERIFY(Profiles); int Index = -1;
          for (int Row = 0; Row < Profiles->count(); ++Row) if (Profiles->item(Row)->data(Qt::UserRole).toString() == IdText(ReferenceProfileId)) Index = Row;
          QVERIFY(Index >= 0); Profiles->setCurrentRow(Index); QCOMPARE(Editor->Profile().id_, ReferenceProfileId);
          QVERIFY(Editor->findChild<QCheckBox*>("hc.columns.splitNameExtension")->isChecked());
          const auto Out = qEnvironmentVariable("HC_UI_ARTIFACT_DIR");
          if (!Out.isEmpty()) QVERIFY(Dialog.grab().save(QDir(Out).filePath("r12-profile-settings-" + QString::number(Dialog.devicePixelRatioF()) + ".png")));
          QVERIFY(Dialog.SaveSettings()); Dialog.reject(); }
        QCOMPARE(View->ColumnProfile().id_, ReferenceProfileId); QCOMPARE(View->CaptureSelection(), QStringList{Key}); QCOMPARE(View->CaptureFocus(), Key);
        QCOMPARE(Model->index(RowOf("report.final.txt"), 0).data().toString(), QString("report.final"));
        QCOMPARE(Model->index(RowOf("report.final.txt"), 0).data(Qt::EditRole).toString(), QString("report.final.txt"));
        QCOMPARE(Model->index(RowOf("README"), 0).data().toString(), QString("README"));
        QVERIFY(View->SetListMode("two-line")); QTRY_VERIFY(View->IsTwoLine());
        QCOMPARE(Model->index(RowOf("report.final.txt"), 0).data().toString(), QString("report.final.txt"));
        QVERIFY(View->SetListMode("one-line")); QTRY_VERIFY(!View->IsTwoLine());
        QCOMPARE(Model->index(RowOf("report.final.txt"), 0).data().toString(), QString("report.final"));
        QVERIFY(Other->ApplyColumnProfile(ReferenceProfile(true)));
        const auto Visible = [](CGridView* Target) {
            QList<QPair<int, QString>> Items; const auto Profile = Target->ColumnProfile();
            for (const auto& Column : Profile.columns_) {
                for (int Index = 0; Index < Target->BaseModel()->columnCount(); ++Index) if (Target->ColumnWidths()->ColumnId(Index) == IdText(Column.id_)) {
                    auto* Native = Target->BaseView()->getColumnByModelColumn(Index); if (Native->isVisible()) Items.append({Native->visualIndex(), Column.fieldId_});
                }
            }
            std::sort(Items.begin(), Items.end()); QStringList Result; for (const auto& Item : Items) Result.append(Item.second); return Result;
        };
        QTRY_COMPARE(Visible(View), (QStringList{"file.name", "file.extension", "file.size", "file.modified", "file.attributes"}));
        QTRY_COMPARE(Visible(Other), (QStringList{"file.name", "file.type", "file.size", "file.modified", "file.attributes"}));
        auto* Bar = main_->findChild<fm::ui::FunctionKeyBar*>("hc.functionBar"); QVERIFY(Bar);
        QCOMPARE(Bar->buttons()[1]->text(), QString("보기")); QVERIFY(Bar->buttons()[1]->accessibleName().contains(Bridge->Action("hc.viewer.open")->property("hc.baseLabel").toString()));
        View->ColumnWidths()->Apply();
        QVERIFY2(View->BaseView()->horizontalScrollBar()->maximum() <= 2, "Reference columns should fit the normal-width pane");
        QVERIFY(Capture("r12-reference-main-light"));
        auto Dark = Colors->GetColorScheme(QStringLiteral("기본 다크")); Dark.FileList_Font.setPixelSize(13); emit Colors->sigColorSchemeChanged(Dark);
        QVERIFY(Capture("r12-reference-main-dark")); emit Colors->sigColorSchemeChanged(Light);
        const auto Saved = View->CaptureWorkspaceView();
        QVERIFY(View->ApplyColumnProfile(DefaultProfile())); View->RestoreWorkspaceView(Saved);
        QTRY_COMPARE(View->ColumnProfile().id_, ReferenceProfileId); QVERIFY(View->ColumnProfile().splitNameExtension_); QCOMPARE(View->CaptureFocus(), Key);
        QCOMPARE(Model->index(RowOf("report.final.txt"), 0).data().toString(), QString("report.final"));
        QVERIFY(View->SetListMode("auto")); main_->resize(900, 650); QTRY_VERIFY(View->IsTwoLine());
        QCOMPARE(Model->index(RowOf("report.final.txt"), 0).data().toString(), QString("report.final.txt")); QVERIFY(Capture("r12-reference-main-narrow")); main_->resize(1420, 900);
        const auto BeforeCaption = Settings.Object(); QPointer<fm::ui::FunctionKeyBar> Same = Bar;
        { QMainOpts Dialog(main_); Dialog.show(); QTest::qWait(80); Dialog.LoadSettings();
          Dialog.findChild<QLineEdit*>("hc.keys.functionCaption1")->setText("내 보기"); Dialog.reject(); }
        QCOMPARE(Settings.Object(), BeforeCaption); QCOMPARE(Bar->buttons()[1]->text(), QString("보기"));
        { QMainOpts Dialog(main_); Dialog.show(); QTest::qWait(80); Dialog.LoadSettings();
          Dialog.findChild<QLineEdit*>("hc.keys.functionCaption1")->setText("내 보기");
          Dialog.findChild<QListWidget*>("lstOptCate")->setCurrentRow(7);
          Dialog.findChild<QWidget*>("hc.settings.keyboard")->findChild<QTabWidget*>()->setCurrentIndex(1);
          QTest::qWait(80);
          const auto Out = qEnvironmentVariable("HC_UI_ARTIFACT_DIR");
          if (!Out.isEmpty()) QVERIFY(Dialog.grab().save(QDir(Out).filePath("r12-function-settings-" + QString::number(Dialog.devicePixelRatioF()) + ".png")));
          QVERIFY(Dialog.SaveSettings()); Dialog.reject(); }
        QVERIFY(Same); QCOMPARE(main_->findChild<fm::ui::FunctionKeyBar*>("hc.functionBar"), Same.data());
        QCOMPARE(Same->buttons()[1]->text(), QString("내 보기"));
        QVERIFY(Same->buttons()[1]->accessibleName().contains(Bridge->Action("hc.viewer.open")->property("hc.baseLabel").toString()));
        QCOMPARE(Settings.Read(hc::settings::Section::General).toObject()["functionCaptions"].toArray()[1].toString(), QString("내 보기"));
        Settings.Reload(); Bridge->RefreshFunctionBar(); QCOMPARE(Same->buttons()[1]->text(), QString("내 보기"));
        QProcess Restart; auto Environment = QProcessEnvironment::systemEnvironment();
        Environment.insert("HC_R12_RESTART", "1"); Environment.insert("HC_TEST_CONFIG_DIR", QFileInfo(Settings.FileName()).absolutePath());
        Restart.setProcessEnvironment(Environment); Restart.start(QCoreApplication::applicationFilePath(), {"r12RestartUi", "-o", "r12-restart.txt,txt"});
        QVERIFY(Restart.waitForStarted(5000)); QVERIFY(Restart.waitForFinished(30000)); QCOMPARE(Restart.exitCode(), 0);
        qInfo() << "R12: presets preserve saved definitions; split/full names, row switches, order, selection, workspace restore, Auto, short captions and caption cancel/apply passed";
    }

    void r12RestartUi()
    {
        if (!qEnvironmentVariableIsSet("HC_R12_RESTART")) QSKIP("Only runs as a preset/caption restart child");
        using namespace hc::columns;
        const auto& Settings = hc::settings::AppSettings();
        const auto Profiles = Settings.Read(hc::settings::Section::Columns).toObject()["ColumnsV2/Profiles"].toObject();
        const auto Saved = DecodeProfile(Profiles[IdText(ReferenceProfileId)].toObject()); QVERIFY(Saved); QVERIFY(Saved->splitNameExtension_);
        QVERIFY(Profiles.contains(IdText(BuiltInProfileId))); QVERIFY(Profiles.contains(IdText(ReferenceTwoLineProfileId)));
        auto* Bar = main_->findChild<fm::ui::FunctionKeyBar*>("hc.functionBar"); QVERIFY(Bar); QCOMPARE(Bar->buttons()[1]->text(), QString("내 보기"));
        QCOMPARE(Settings.Read(hc::settings::Section::General).toObject()["functionCaptions"].toArray()[1].toString(), QString("내 보기"));
    }

    void listPresentationUi()
    {
        using namespace fm::filelist;
        QVERIFY(ResolveAutoTwoLine(639, 0, 640, 25, false, false));
        QVERIFY(!ResolveAutoTwoLine(640, 0, 640, 25, false, false));
        QVERIFY(!ResolveAutoTwoLine(641, 0, 640, 25, false, false));
        QVERIFY(ResolveAutoTwoLine(641, 0, 640, 25, true, true));
        QVERIFY(!ResolveAutoTwoLine(672, 0, 640, 25, true, true));
        QVERIFY(ResolveAutoTwoLine(900, 25, 640, 25, false, false));
        QVERIFY(ResolveAutoTwoLine(900, 20, 640, 25, true, true));
        QVERIFY(!ResolveAutoTwoLine(900, 19.9, 640, 25, true, true));
        QVERIFY(!ResolveAutoTwoLine(900, 0, 640, 5, true, true));
        const QFontMetrics Metrics(QFont("Segoe UI", 10));
        const auto LongName = QString::fromUtf8("긴 한글 파일 이름과 여러 구성요소를 포함한 압축 자료.tar.gz");
        const auto Elided = ElideFileName(LongName, Metrics, 160);
        QVERIFY(Elided.endsWith(".tar.gz")); QVERIFY(Elided.contains(QChar(0x2026)));
        QVERIFY(Metrics.horizontalAdvance(Elided) <= 160);
        QCOMPARE(ElideFileName("README", Metrics, 500), QString("README"));
        QCOMPARE(ElideFileName(".gitignore", Metrics, 500), QString(".gitignore"));

        auto& Settings = hc::settings::AppSettings(); const auto Original = Settings.Object();
        const auto Workspace = main_->CaptureWorkspace();
        auto* Colors = TyStColorSchemeMgr::GetInstance(); const auto Scheme = Colors->GetColorScheme(Colors->GetCurrentColorScheme());
        const auto Restore = qScopeGuard([&] {
            Settings.DiscardEdit(); Settings.Replace(Original); Settings.Commit();
            emit Colors->sigColorSchemeChanged(Scheme); main_->RestoreWorkspace(Workspace); main_->SaveWorkspace();
        });
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        for (int Index = 0; Index < 180; ++Index) {
            QFile File(Directory.filePath(QString("item-%1.txt").arg(Index, 3, 10, QChar('0'))));
            QVERIFY(File.open(QIODevice::WriteOnly)); File.write("R3");
        }
        { QFile File(Directory.filePath(LongName)); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("R3"); }
        hc::workspace::TabState Tab; Tab.id_ = "r3-list";
        Tab.location_ = {hc::storage::LocatorKind::Local, Directory.path().toStdString()};
        auto* Panel = main_->findChild<CmpPanel*>("cmpLeftPanel"); Panel->RestoreWorkspace({{Tab}, 0});
        QTRY_VERIFY_WITH_TIMEOUT(!model(Panel)->IsLoading() && model(Panel)->rowCount() == 182, 10000);
        auto* View = static_cast<CGridView*>(Panel->GetFocusView(0));
        auto* Native = View->BaseView(); auto* Grid = View->findChild<Qtitan::Grid*>();
        auto Light = Colors->GetColorScheme(QStringLiteral("기본 라이트")); Light.FileList_Font.setPixelSize(13);
        emit Colors->sigColorSchemeChanged(Light); main_->resize(1360, 850); Panel->sig_NotifyPanelActivated();
        auto General = Settings.Read(hc::settings::Section::General).toObject();
        General["filePanel"] = QJsonObject{{"version", 1}, {"defaultMode", "one-line"}};
        QVERIFY(Settings.Put(hc::settings::Section::General, General)); View->ApplyDisplaySettings();
        QVERIFY(View->SetListMode("one-line")); QTest::qWait(180);
        Native->setFocusedRowIndex(100); Native->selectRow(100); Native->selectRow(101); QTest::qWait(50);
        const auto Focus = View->CaptureFocus(); const auto Selection = View->CaptureSelection();
        const auto Sort = View->ColumnProfile().sorts_;
        const auto BeforeViewport = View->ColumnWidths()->CaptureViewport();
        QVERIFY(Native->verticalScrollBar()->value() > 0);
        QVERIFY(View->SetListMode("two-line")); QTRY_VERIFY(View->IsTwoLine()); QTest::qWait(180);
        QCOMPARE(View->CaptureFocus(), Focus); QCOMPARE(View->CaptureSelection(), Selection); QVERIFY(View->ColumnProfile().sorts_ == Sort);
        const auto AfterViewport = View->ColumnWidths()->CaptureViewport();
        QCOMPARE(AfterViewport.entry_, BeforeViewport.entry_); QCOMPARE(AfterViewport.offset_, BeforeViewport.offset_);
        QVERIFY(Native->getColumnByModelColumn(0)->width() >= View->ColumnWidths()->AvailableWidth());
        auto* Name = static_cast<Qtitan::GridBandedTableColumn*>(Native->getColumnByModelColumn(0));
        QCOMPARE(Name->rowIndex(), 0);
        bool FoundType = false;
        for (int Column = 0; Column < model(Panel)->columnCount(); ++Column) {
            auto* Meta = static_cast<Qtitan::GridBandedTableColumn*>(Native->getColumnByModelColumn(Column));
            if (Column != 0 && Meta->isVisible()) QCOMPARE(Meta->rowIndex(), 1);
            for (const auto& Definition : View->ColumnProfile().columns_) if (Definition.fieldId_ == "file.type" &&
                View->ColumnWidths()->ColumnId(Column) == hc::columns::IdText(Definition.id_)) FoundType = Meta->isVisible();
        }
        QVERIFY(FoundType);
        QVERIFY(!model(Panel)->index(100, 0).data(Qt::DecorationRole).value<QPixmap>().isNull());
        for (int Column = 0; Column < model(Panel)->columnCount(); ++Column)
            for (const auto& Definition : View->ColumnProfile().columns_)
                if (Definition.fieldId_ == "file.type" && View->ColumnWidths()->ColumnId(Column) == hc::columns::IdText(Definition.id_)) {
                    QVERIFY(Native->getColumnByModelColumn(Column)->width() >= 80);
                    QTRY_VERIFY(!model(Panel)->index(100, Column).data().toString().isEmpty());
                }

        // Observe actual painted record coordinates; both visual lines focus the same Entry.
        struct Probe final : Qtitan::GridRecordPainter {
            Qtitan::GridRecordPainter* wrapped_;
            QMap<int, QRect> records_;
            explicit Probe(Qtitan::GridRecordPainter* Wrapped) : wrapped_(Wrapped) {}
            void paintBackground(QPainter& Painter, const Record& Record, QWidget* Widget) override {
                records_[Record.rowIndex] = Record.rect; wrapped_->paintBackground(Painter, Record, Widget);
            }
            void paintOverlay(QPainter& Painter, const Record& Record, QWidget* Widget) override { wrapped_->paintOverlay(Painter, Record, Widget); }
        } Observer(Native->recordPainter());
        Native->scrollToRow(Native->focusedRow(), false);
        Native->setRecordPainter(&Observer);
        { const auto Undo = qScopeGuard([&] { Native->setRecordPainter(Observer.wrapped_); });
          Grid->viewport()->repaint(); QTRY_VERIFY(Observer.records_.contains(100));
          const auto Rect = Observer.records_.value(100);
          QTest::mouseClick(Grid->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(Rect.left() + 180, Rect.top() + Rect.height() / 4));
          QCOMPARE(View->CaptureFocus(), Focus);
          QTest::mouseClick(Grid->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(Rect.left() + 180, Rect.top() + 3 * Rect.height() / 4));
          QCOMPARE(View->CaptureFocus(), Focus);
        }
        General["filePanel"] = QJsonObject{{"version", 1}, {"defaultMode", "two-line"}, {"nameBelow", true},
            {"autoWidthDip", View->ColumnWidths()->AvailableWidth() + 1}, {"autoClippedPercent", 100}, {"twoLineSeparator", "tint"}};
        QVERIFY(Settings.Put(hc::settings::Section::General, General)); View->ApplyDisplaySettings();
        QVERIFY(View->SetListMode("inherit")); QTRY_COMPARE(Name->rowIndex(), 1);
        QCOMPARE(View->ListModeOverride(), QString("inherit")); QCOMPARE(View->ListMode(), QString("two-line"));
        QVERIFY(View->SetListMode("one-line")); View->ApplyDisplaySettings(); QTest::qWait(120); QVERIFY(!View->IsTwoLine());
        QVERIFY(View->SetListMode("auto")); QTRY_VERIFY(View->IsTwoLine()); QCOMPARE(View->ListMode(), QString("auto"));
        auto PanelOptions = General["filePanel"].toObject(); PanelOptions["autoWidthDip"] = 100; General["filePanel"] = PanelOptions;
        QVERIFY(Settings.Put(hc::settings::Section::General, General)); View->ApplyDisplaySettings();
        QTRY_VERIFY(!View->IsTwoLine()); QCOMPARE(View->ListMode(), QString("auto"));
        const auto Saved = View->CaptureWorkspaceView(); QCOMPARE(Saved["listPresentation"].toObject()["mode"].toString(), QString("auto"));
        QVERIFY(View->SetListMode("two-line")); View->RestoreWorkspaceView(Saved); QTRY_VERIFY(!View->IsTwoLine()); QCOMPARE(View->ListMode(), QString("auto"));
        // Restoring a profile recreates Qtitan columns; reacquire the new name column.
        Name = static_cast<Qtitan::GridBandedTableColumn*>(Native->getColumnByModelColumn(0));

        const auto BeforeSettings = Settings.Read(hc::settings::Section::General);
        { QMainOpts Dialog(main_); Dialog.SetListView(View); Dialog.show(); QCoreApplication::processEvents();
          Dialog.findChild<QComboBox*>("hc.settings.listDefault")->setCurrentIndex(1);
          Dialog.findChild<QPushButton*>("hc.settings.useListDefault")->click();
          QCOMPARE(View->ListModeOverride(), QString("auto")); Dialog.reject();
        }
        QCOMPARE(Settings.Read(hc::settings::Section::General), BeforeSettings); QCOMPARE(View->ListModeOverride(), QString("auto"));
        { QMainOpts Dialog(main_); Dialog.SetListView(View); Dialog.show(); QCoreApplication::processEvents();
          Dialog.findChild<QComboBox*>("hc.settings.listDefault")->setCurrentIndex(1);
          Dialog.findChild<QCheckBox*>("hc.settings.listNameBelow")->setChecked(true);
          Dialog.findChild<QPushButton*>("hc.settings.useListDefault")->click();
          QVERIFY(Dialog.SaveSettings()); QTRY_VERIFY(View->IsTwoLine()); QCOMPARE(View->ListModeOverride(), QString("inherit")); QCOMPARE(Name->rowIndex(), 1);
          const auto Artifacts = qEnvironmentVariable("HC_UI_ARTIFACT_DIR");
          if (!Artifacts.isEmpty()) { Dialog.findChild<QListWidget*>("lstOptCate")->setCurrentRow(2); QCoreApplication::processEvents(); auto* Page = Dialog.findChild<QWidget*>("hc.settings.listPresentation"); QVERIFY(Page); QVERIFY(Page->grab().save(QDir(Artifacts).filePath("r3-file-panel-settings.png"))); }
          Dialog.reject();
        }
        QCOMPARE(Settings.Read(hc::settings::Section::General).toObject()["filePanel"].toObject()["defaultMode"].toString(), QString("two-line"));
        const auto Persisted = main_->CaptureWorkspace(); main_->RestoreWorkspace(hc::workspace::Decode(hc::workspace::Encode(Persisted)).state_);
        View = static_cast<CGridView*>(Panel->GetFocusView(0)); QTRY_VERIFY(!model(Panel)->IsLoading());
        QCOMPARE(View->ListModeOverride(), QString("inherit")); QTRY_VERIFY(View->IsTwoLine());
        auto* Controls = Panel->findChild<fm::ui::SegmentedControl*>("hc.panelViewMode"); QVERIFY(Controls->button(2)->isEnabled()); QVERIFY(Controls->button(3)->isEnabled());
        const auto Artifacts = qEnvironmentVariable("HC_UI_ARTIFACT_DIR");
        if (!Artifacts.isEmpty()) {
            auto CurrentGeneral = Settings.Read(hc::settings::Section::General).toObject();
            auto Presentation = CurrentGeneral["filePanel"].toObject();
            Presentation["nameBelow"] = false; Presentation["twoLineSeparator"] = "line";
            CurrentGeneral["filePanel"] = Presentation; QVERIFY(Settings.Put(hc::settings::Section::General, CurrentGeneral));
            View->ApplyDisplaySettings();
            auto Capture = [&](const QString& Name) { QTest::qWait(150); return main_->grab().save(QDir(Artifacts).filePath(Name)); };
            emit Colors->sigColorSchemeChanged(Light); View->SetListMode("one-line"); QVERIFY(Capture("r3-main-one-line.png"));
            View->SetListMode("two-line"); QVERIFY(Capture("r3-main-two-line.png"));
            auto Dark = Colors->GetColorScheme(QStringLiteral("기본 다크")); Dark.FileList_Font.setPixelSize(13);
            emit Colors->sigColorSchemeChanged(Dark); QVERIFY(Capture("r3-main-dark.png"));
            emit Colors->sigColorSchemeChanged(Light); View->SetListMode("auto"); main_->resize(900, 650); QVERIFY(Capture("r3-main-auto-narrow.png"));
        }
        const QJsonObject Future{{"version", 99}, {"mode", "future"}};
        View->RestoreWorkspaceView({{"listPresentation", Future}}); QCOMPARE(View->CaptureWorkspaceView()["listPresentation"].toObject(), Future);
        QVERIFY(!View->SetListMode("auto"));
        qInfo() << "R3: geometry, metadata, both-line hit testing, selection/focus/sort/viewport, Auto thresholds/hysteresis, extension elide, tab/default precedence, settings cancel/apply and workspace persistence passed";
    }

    void mainPanelChromeUi()
    {
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        const auto Root = QDir::toNativeSeparators(Directory.path());
        QDir Dir(Root); QVERIFY(Dir.mkpath("A")); QVERIFY(Dir.mkpath("B")); QVERIFY(Dir.mkpath("C"));
        QFile Item(Dir.filePath("A/sample.txt")); QVERIFY(Item.open(QIODevice::WriteOnly)); Item.write("sample"); Item.close();
        for (const auto& Name : {QString("C/report.txt"), QString("C/design.png"), QString("C/긴 한글 파일 이름을 확인하는 자료.pdf")}) {
            QFile File(Dir.filePath(Name)); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("r2 fixture");
        }
        const auto Previous = main_->CaptureWorkspace();
        const auto Restore = qScopeGuard([&] { main_->showNormal(); main_->RestoreWorkspace(Previous); });
        using namespace hc::workspace;
        State Value;
        TabState A; A.id_ = "r2-left-a"; A.location_ = {hc::storage::LocatorKind::Local, QDir::toNativeSeparators(Dir.filePath("A")).toStdString()};
        auto B = A; B.id_ = "r2-left-b"; B.location_.root_ = QDir::toNativeSeparators(Dir.filePath("B")).toStdString();
        auto C = A; C.id_ = "r2-right-c"; C.location_.root_ = QDir::toNativeSeparators(Dir.filePath("C")).toStdString();
        Value.left_.tabs_ = {A, B}; Value.right_.tabs_ = {C}; main_->RestoreWorkspace(Value);
        auto* Left = main_->findChild<CmpPanel*>("cmpLeftPanel"); auto* Right = main_->findChild<CmpPanel*>("cmpRightPanel");
        QTRY_VERIFY_WITH_TIMEOUT(!model(Left)->IsLoading() && !model(Right)->IsLoading(), 10000);
        auto* Strip = Left->findChild<fm::ui::PanelTabStrip*>("hc.panelTabs"); QVERIFY(Strip);
        auto* Tabs = Strip->tabBar(); QCOMPARE(Tabs->count(), 2); QCOMPARE(Tabs->tabData(0).toString(), A.id_);
        QVERIFY(Strip->geometry().top() < Left->findChild<QFrame*>("frmHDR")->geometry().top());
        QVERIFY(main_->findChild<fm::ui::WindowChrome*>());
        auto* Find = main_->findChild<fm::ui::FindBox*>("hc.mainFind"); QVERIFY(Find);
        Left->sig_NotifyPanelActivated(); Find->setText("ext:txt"); QCOMPARE(Left->Search(), QString("ext:txt")); QCOMPARE(Right->Search(), QString{});
        Tabs->setCurrentIndex(1); QTRY_VERIFY_WITH_TIMEOUT(!model(Left)->IsLoading(), 10000);
        QCOMPARE(Find->text(), QString{}); Find->setText("name:second");
        Tabs->setCurrentIndex(0); QTRY_VERIFY_WITH_TIMEOUT(!model(Left)->IsLoading(), 10000); QCOMPARE(Find->text(), QString("ext:txt"));
        const auto PathA = QString::fromStdString(A.location_.root_); const auto PathB = QString::fromStdString(B.location_.root_); const auto PathC = QString::fromStdString(C.location_.root_);
        Left->ChangeCurrentPath(PathB, {}); QTRY_COMPARE_WITH_TIMEOUT(Left->CurrentPath(), PathB, 10000); QTRY_VERIFY(!model(Left)->IsLoading());
        Left->ChangeCurrentPath(PathC, {}); QTRY_COMPARE_WITH_TIMEOUT(Left->CurrentPath(), PathC, 10000); QTRY_VERIFY(!model(Left)->IsLoading());
        QVERIFY(Left->CanNavigateHistory(-1)); TyStCommandMgr::GetInstance()->Bridge()->ExecuteCommand("hc.navigate.back");
        QTRY_COMPARE_WITH_TIMEOUT(Left->CurrentPath(), PathB, 10000); QTRY_VERIFY(!model(Left)->IsLoading());
        QVERIFY(Left->CanNavigateHistory(1)); Left->NavigateHistory(1);
        QTRY_COMPARE_WITH_TIMEOUT(Left->CurrentPath(), PathC, 10000); QTRY_VERIFY(!model(Left)->IsLoading());
        Left->NavigateHistory(-1); QTRY_COMPARE_WITH_TIMEOUT(Left->CurrentPath(), PathB, 10000); QTRY_VERIFY(!model(Left)->IsLoading());
        Left->ChangeCurrentPath(PathA, {}); QTRY_COMPARE_WITH_TIMEOUT(Left->CurrentPath(), PathA, 10000); QTRY_VERIFY(!model(Left)->IsLoading());
        QVERIFY(!Left->CanNavigateHistory(1)); QCOMPARE(Left->Search(), QString("ext:txt"));
        const auto SavedBeforeMove = Left->CaptureWorkspace();
        Tabs->moveTab(0, 1); QCOMPARE(Left->CaptureWorkspace().tabs_[1].id_, A.id_);
        QCOMPARE(Left->CaptureWorkspace().tabs_[1].view_.value("search").toString(), QString("ext:txt"));
        const auto Saved = main_->CaptureWorkspace(); QVERIFY(Decode(Encode(Saved)).writable_);
        main_->RestoreWorkspace(Decode(Encode(Saved)).state_); QTRY_VERIFY(!model(Left)->IsLoading());
        QCOMPARE(Tabs->tabData(1).toString(), A.id_); QCOMPARE(Left->Search(), QString("ext:txt"));
        QTest::mouseClick(Tabs, Qt::MiddleButton, Qt::NoModifier, Tabs->tabRect(0).center());
        QCOMPARE(Left->CaptureWorkspace().tabs_.size(), 1); QCOMPARE(Left->CaptureWorkspace().tabs_[0].id_, A.id_);
        auto* CommandLine = main_->findChild<fm::ui::CommandLine*>("hc.commandLine"); QVERIFY(CommandLine);
        Left->sig_NotifyPanelActivated(); QVERIFY(CommandLine->prompt().contains("A")); QVERIFY(main_->windowTitle().contains(PathA));
        CommandLine->lineEdit()->setText("echo r2>r2-command.txt"); QTest::keyClick(CommandLine->lineEdit(), Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(Dir.filePath("A/r2-command.txt")), 10000);
        QTRY_VERIFY(CommandLine->lineEdit()->isEnabled());
        CommandLine->lineEdit()->setText("exit /b 23"); QTest::keyClick(CommandLine->lineEdit(), Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(main_->statusBar()->currentMessage().contains("23"), 10000);
        QVERIFY(CommandLine->history().contains("exit /b 23"));
        const auto Cursor = Left->CaptureWorkspace().tabs_[0].historyIndex_;
        Left->ChangeCurrentPath(Dir.filePath("missing"), {}); QTRY_VERIFY_WITH_TIMEOUT(bool(static_cast<CFSModel*>(model(Left))->LastNavigationError()), 10000);
        QCOMPARE(Left->CaptureWorkspace().tabs_[0].historyIndex_, Cursor);
        QVERIFY(Left->findChild<QPushButton*>("hc.workspaceRetry")->isVisible());
        QVERIFY(Dir.mkdir("missing")); Left->findChild<QPushButton*>("hc.workspaceRetry")->click(); QTRY_VERIFY(!model(Left)->IsLoading());
        QTRY_VERIFY(!static_cast<CFSModel*>(model(Left))->LastNavigationError());
        Left->ChangeCurrentPath(PathA, {}); QTRY_VERIFY(!model(Left)->IsLoading());
        Find->clear(); Left->AddTab(); QTRY_VERIFY(!model(Left)->IsLoading());
        QCOMPARE(Left->CaptureWorkspace().tabs_.size(), 2); QCOMPARE(Right->CaptureWorkspace().tabs_.size(), 1);
        auto* RightMode = Right->findChild<fm::ui::SegmentedControl*>("hc.panelViewMode"); QVERIFY(RightMode);
        RightMode->setCurrentIndex(1); QCOMPARE(static_cast<CGridView*>(Right->GetFocusView(Right->CurrentTabIndex()))->ColumnWidths()->LayoutMode(), QString("name-first"));
        QVERIFY(RightMode->button(2)->isEnabled()); QVERIFY(RightMode->button(3)->isEnabled());
        Left->sig_NotifyPanelActivated();
        // Simulate the device notification without removing a user's real volume.
        Left->NotifyDriveRemoved(Root.left(3)); QVERIFY(static_cast<CFSModel*>(model(Left))->HasPlaceholder());
        QVERIFY(Left->findChild<QPushButton*>("hc.workspaceRetry")->isVisible());
        Left->RefreshSource(Left->CurrentTabIndex()); QTRY_VERIFY(!model(Left)->IsLoading());
        QTRY_VERIFY(!static_cast<CFSModel*>(model(Left))->HasPlaceholder()); QCOMPARE(Left->CurrentPath(), PathA);
        main_->resize(1360, 850);
        const auto Artifacts = qEnvironmentVariable("HC_UI_ARTIFACT_DIR");
        if (!Artifacts.isEmpty()) {
            QDir().mkpath(Artifacts); QTest::qWait(100);
            QVERIFY(main_->grab().save(QDir(Artifacts).filePath("r2-main-light.png")));
            auto* Colors = TyStColorSchemeMgr::GetInstance();
            emit Colors->sigColorSchemeChanged(Colors->GetColorScheme(QStringLiteral("기본 다크"))); QTest::qWait(100);
            QVERIFY(main_->grab().save(QDir(Artifacts).filePath("r2-main-dark.png")));
            emit Colors->sigColorSchemeChanged(Colors->GetColorScheme(QStringLiteral("기본 라이트")));
            main_->resize(900, 650); QTest::qWait(100);
            QVERIFY(main_->grab().save(QDir(Artifacts).filePath("r2-main-narrow.png")));
        }
        main_->showMaximized(); QTRY_VERIFY(main_->isMaximized()); main_->showNormal(); QTRY_VERIFY(!main_->isMaximized());
        auto* Chrome = main_->findChild<fm::ui::WindowChrome*>();
        auto* Maximize = Chrome->findChild<QToolButton*>("fm.window.maximize"); QVERIFY(Maximize);
        const auto Handle = reinterpret_cast<HWND>(main_->winId());
        const auto Flags = GetWindowLongPtrW(Handle, GWL_STYLE);
        QVERIFY(Flags & WS_THICKFRAME); QVERIFY(Flags & WS_MAXIMIZEBOX); QVERIFY(GetSystemMenu(Handle, FALSE));
        const auto Local = Maximize->mapTo(main_, Maximize->rect().center());
        const auto Scale = main_->devicePixelRatioF(); POINT Native{qRound(Local.x() * Scale), qRound(Local.y() * Scale)}; ClientToScreen(Handle, &Native);
        QCOMPARE(SendMessageW(Handle, WM_NCHITTEST, 0, MAKELPARAM(Native.x, Native.y)), LRESULT(HTMAXBUTTON));
        // Nonclient mouse input normally enters Qt through its event dispatcher.
        // Post it to the native queue instead of bypassing that route with SendMessage.
        PostMessageW(Handle, WM_NCLBUTTONDOWN, HTMAXBUTTON, MAKELPARAM(Native.x, Native.y));
        PostMessageW(Handle, WM_NCLBUTTONUP, HTMAXBUTTON, MAKELPARAM(Native.x, Native.y)); QTRY_VERIFY(main_->isMaximized());
        main_->showNormal();
        qInfo() << "R2: independent tab search, identity reorder/close, history branching, missing-path retry, shell cwd/exit, workspace restoration and maximize/restore passed";
    }

    void fileIconRefreshUi()
    {
        auto& Settings = hc::settings::AppSettings(); const auto Original = Settings.Object();
        const auto Workspace = main_->CaptureWorkspace();
        const auto Restore = qScopeGuard([&] {
            Settings.DiscardEdit(); Settings.Replace(Original); Settings.Commit(); main_->RestoreWorkspace(Workspace);
        });
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        const auto IconPath = Directory.filePath("!rebuild.ico");
        QImage Red(32, 32, QImage::Format_ARGB32); Red.fill(Qt::red); QVERIFY(Red.save(IconPath, "ICO"));
        for (int At = 0; At < 200; ++At) { QFile File(Directory.filePath(QString("file%1.txt").arg(At))); QVERIFY(File.open(QIODevice::WriteOnly)); }
        auto General = Settings.Read(hc::settings::Section::General).toObject();
        General["iconMode"] = "all"; General["iconOverlays"] = false; QVERIFY(Settings.Put(hc::settings::Section::General, General));
        auto* Panel = panels_.front(); Panel->SetSearch({}); Panel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {});
        auto* Model = static_cast<CFSModel*>(model(Panel));
        auto* View = qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex()));
        QVERIFY(View); View->SetListMode("one-line");
        QTRY_VERIFY(!Model->IsLoading() && names(Model).contains("!rebuild.ico")); Model->ApplyDisplaySettings(); Model->SetWatchEnabled(false);
        const auto Image = [&] {
            for (int Row = 0; Row < Model->rowCount(); ++Row) if (Model->GetName(Model->index(Row, 0)) == "!rebuild.ico")
                return qvariant_cast<QPixmap>(Model->data(Model->index(Row, 0), Qt::DecorationRole)).toImage();
            return QImage{};
        };
        const auto Color = [&] { const auto Value = Image(); return Value.isNull() ? QColor{} : Value.pixelColor(Value.width() / 2, Value.height() / 2); };
        const auto Near = [&](Qt::GlobalColor Expected) {
            const auto Actual = Color(); const QColor Target(Expected);
            return Actual.isValid() && std::abs(Actual.red() - Target.red()) <= 2 && std::abs(Actual.green() - Target.green()) <= 2
                && std::abs(Actual.blue() - Target.blue()) <= 2 && std::abs(Actual.alpha() - Target.alpha()) <= 2;
        };
        // Windows resampling at non-integral icon sizes can round an 8-bit channel by one.
        QTRY_VERIFY_WITH_TIMEOUT(Near(Qt::red), 10000);
        auto* Bridge = TyStCommandMgr::GetInstance()->Bridge();
        QVERIFY(Bridge->Action("hc.navigate.forceRefresh"));
        auto Warm = Bridge->Dispatch("hc.navigate.refresh", {}, "test", Model->index(0, 0));
        QTRY_VERIFY(Warm.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready); QVERIFY(Warm.get().HasValue());
        QTRY_VERIFY(!Model->IsLoading()); QVERIFY(Near(Qt::red));
        const QFileInfo Before(IconPath); const auto Modified = Before.lastModified(); const auto Size = Before.size();
        QImage Blue(32, 32, QImage::Format_ARGB32); Blue.fill(Qt::blue); QVERIFY(Blue.save(IconPath, "ICO"));
        { QFile File(IconPath); QVERIFY(File.open(QIODevice::ReadWrite)); QVERIFY(File.setFileTime(Modified, QFileDevice::FileModificationTime)); }
        QCOMPARE(QFileInfo(IconPath).size(), Size);
        auto Force = Bridge->Dispatch("hc.navigate.forceRefresh", {}, "test", Model->index(0, 0));
        QTRY_VERIFY(Force.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready); QVERIFY(Force.get().HasValue());
        QTRY_VERIFY_WITH_TIMEOUT(Near(Qt::blue), 10000);
        const auto& Stats = hc::icons::FileIconService::Instance().Stats();
        QVERIFY(Stats.maxPending_ <= 1024);
        qInfo() << "Icon service loads/hits/merged/conversions/maxApplyUs:" << Stats.loads_ << Stats.hits_ << Stats.merged_ << Stats.conversions_ << Stats.maxApplyUs_;
    }

    void todoBugFolderShellIconsUi()
    {
        auto& Settings = hc::settings::AppSettings(); const auto Original = Settings.Object();
        const auto Workspace = main_->CaptureWorkspace();
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        const auto Custom = QDir::toNativeSeparators(Directory.filePath("custom"));
        const auto Restore = qScopeGuard([&] {
            SetFileAttributesW(Custom.toStdWString().c_str(), FILE_ATTRIBUTE_NORMAL);
            Settings.DiscardEdit(); Settings.Replace(Original); Settings.Commit();
            auto* Colors = TyStColorSchemeMgr::GetInstance();
            emit Colors->sigColorSchemeChanged(Colors->GetColorScheme(Colors->GetCurrentColorScheme()));
            main_->RestoreWorkspace(Workspace);
        });
        QVERIFY(QDir(Directory.path()).mkdir("custom")); QVERIFY(QDir(Directory.path()).mkdir("plain"));
        const auto Ini = Custom + "\\desktop.ini";
        { QFile File(Ini); QVERIFY(File.open(QIODevice::WriteOnly));
          QVERIFY(File.write("[.ShellClassInfo]\r\nIconResource=%SystemRoot%\\System32\\shell32.dll,15\r\n") > 0); }
        QVERIFY(SetFileAttributesW(Ini.toStdWString().c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM));
        QVERIFY(SetFileAttributesW(Custom.toStdWString().c_str(), FILE_ATTRIBUTE_READONLY));
        for (int Index = 0; Index < 70; ++Index) {
            QFile File(Directory.filePath(QString("item%1.tmp").arg(Index, 2, 10, QChar('0')))); QVERIFY(File.open(QIODevice::WriteOnly));
        }
        // A real Shell link exercises the overlay handler without changing any user files.
        IShellLinkW* Link = nullptr; QVERIFY(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&Link))));
        const auto FreeLink = qScopeGuard([&] { Link->Release(); });
        QVERIFY(SUCCEEDED(Link->SetPath(QDir::toNativeSeparators(Directory.filePath("plain")).toStdWString().c_str())));
        IPersistFile* Persist = nullptr; QVERIFY(SUCCEEDED(Link->QueryInterface(IID_PPV_ARGS(&Persist))));
        const auto FreePersist = qScopeGuard([&] { Persist->Release(); });
        QVERIFY(SUCCEEDED(Persist->Save(QDir::toNativeSeparators(Directory.filePath("link.lnk")).toStdWString().c_str(), TRUE)));
        auto General = Settings.Read(hc::settings::Section::General).toObject();
        General["iconMode"] = "standard"; General["specialFolderIcons"] = true;
        General["virtualFolderIcons"] = true; General["explorerDefaultIcons"] = true; General["iconOverlays"] = false;
        QVERIFY(Settings.Put(hc::settings::Section::General, General));
        auto* Panel = panels_.front(); Panel->SetSearch({}); Panel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {});
        auto* Model = static_cast<CFSModel*>(model(Panel));
        QTRY_VERIFY(!Model->IsLoading() && names(Model).contains("link.lnk")); Model->ApplyDisplaySettings();
        const auto Find = [&](const QString& Name) {
            for (int Row = 0; Row < Model->rowCount(); ++Row) if (Model->GetName(Model->index(Row, 0)) == Name) return Model->index(Row, 0);
            return QModelIndex{};
        };
        const auto Image = [&](const QString& Name) { return qvariant_cast<QPixmap>(Model->data(Find(Name), Qt::DecorationRole)).toImage(); };
        auto* IconView = qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex())); QVERIFY(IconView);
        const auto ShowIcon = [&](const QString& Name) {
            const auto Source = Find(Name); auto* Proxy = qobject_cast<QAbstractProxyModel*>(IconView->BaseView()->model());
            IconView->BaseView()->scrollToIndex(Proxy ? Proxy->mapFromSource(Source) : Source, false);
        };
        QTRY_VERIFY_WITH_TIMEOUT(!Image("custom").isNull() && Image("custom") != Image("plain"), 10000);
        QMainOpts Dialog(main_); Dialog.show(); QTest::qWait(50); Dialog.LoadSettings();
        auto* Categories = Dialog.findChild<QListWidget*>("lstOptCate");
        const auto Pages = Categories->findItems(QStringLiteral("파일 패널"), Qt::MatchExactly); QCOMPARE(Pages.size(), 1);
        Categories->setCurrentItem(Pages.front());
        auto* Tabs = Dialog.findChild<QTabWidget*>("hc.settings.filePanelTabs"); QVERIFY(Tabs);
        QCOMPARE(Tabs->count(), 2); QCOMPARE(Tabs->tabText(0), QStringLiteral("공통")); QCOMPARE(Tabs->tabText(1), QStringLiteral("아이콘"));
        const auto Check = [&](const char* Key) { return Dialog.findChild<QCheckBox*>("hc.settings." + QString::fromLatin1(Key)); };
        auto* Special = Check("specialFolderIcons"); QVERIFY(Special);
        Tabs->setCurrentIndex(1); QTRY_VERIFY(Special->isVisible());
        QVERIFY(Dialog.findChild<QRadioButton*>("radioButton_5")->isVisible());
        QVERIFY(Dialog.findChild<QCheckBox*>("chkIsDimmedIconWhenHidden")->isVisible());
        QVERIFY(!Dialog.findChild<QCheckBox*>("chkSortNumericAware")->isVisible());
        ShowIcon("link.lnk");
        hc::icons::Request GenericFile; GenericFile.pixels_ = Model->IconPixels();
        QTRY_VERIFY(!hc::icons::FileIconService::Instance().Read(GenericFile).pixmap_.isNull());
        QTRY_COMPARE(Image("link.lnk"), hc::icons::FileIconService::Instance().Read(GenericFile).pixmap_.toImage());
        const auto NoOverlay = Image("link.lnk");
        Check("iconOverlays")->setChecked(true); QVERIFY(Dialog.SaveSettings());
        QTRY_VERIFY_WITH_TIMEOUT(Image("link.lnk") != NoOverlay, 10000);
        QVERIFY(main_->grab().save(QDir(QCoreApplication::applicationDirPath()).filePath("folder-shell-icons.png")));
        Check("iconOverlays")->setChecked(false); Special->setChecked(false); QVERIFY(Dialog.SaveSettings());
        QTRY_COMPARE(Image("link.lnk"), NoOverlay);
        ShowIcon("custom"); QTRY_COMPARE(Image("custom"), Image("plain"));
        Check("explorerDefaultIcons")->setChecked(false); Check("virtualFolderIcons")->setChecked(false); QVERIFY(Dialog.SaveSettings());
        QTRY_COMPARE(Image("plain"), QApplication::style()->standardIcon(QStyle::SP_DirIcon).pixmap(QSize(Model->IconPixels(), Model->IconPixels()), 1.0)
            .scaled(Model->IconPixels(), Model->IconPixels(), Qt::KeepAspectRatio, Qt::SmoothTransformation).toImage());
        Check("explorerDefaultIcons")->setChecked(true); Special->setChecked(true); QVERIFY(Dialog.SaveSettings());
        QTRY_VERIFY_WITH_TIMEOUT(Image("custom") != Image("plain"), 10000);
        Dialog.findChild<QRadioButton*>("radioButton_4")->setChecked(true);
        QVERIFY(!Special->isEnabled()); QVERIFY(Special->isChecked()); QVERIFY(Dialog.SaveSettings());
        QVERIFY(!Model->data(Find("custom"), Qt::DecorationRole).isValid());
        Dialog.findChild<QRadioButton*>("radioButton_5")->setChecked(true); QVERIFY(Special->isEnabled()); QVERIFY(Dialog.SaveSettings());
        QTRY_VERIFY_WITH_TIMEOUT(!Image("custom").isNull() && Image("custom") != Image("plain"), 10000);
        const auto Artifacts = QDir(QCoreApplication::applicationDirPath());
        QVERIFY(Dialog.grab().save(Artifacts.filePath("file-panel-icons.png")));
        Tabs->setCurrentIndex(0); QVERIFY(!Special->isVisible());
        QVERIFY(Dialog.findChild<QCheckBox*>("chkSortNumericAware")->isVisible());
        Dialog.resize(900, 650); QTest::qWait(80); QVERIFY(Dialog.grab().save(Artifacts.filePath("file-panel-common.png")));
        Dialog.reject(); Settings.Reload();
        { QMainOpts Reopen(main_); Reopen.show(); QTest::qWait(50); Reopen.LoadSettings();
          QVERIFY(Reopen.findChild<QCheckBox*>("hc.settings.specialFolderIcons")->isChecked());
          QVERIFY(!Reopen.findChild<QCheckBox*>("hc.settings.virtualFolderIcons")->isChecked());
          QVERIFY(!Reopen.findChild<QCheckBox*>("hc.settings.iconOverlays")->isChecked());
          Reopen.findChild<QCheckBox*>("hc.settings.specialFolderIcons")->setChecked(false); Reopen.reject(); }
        QVERIFY(Settings.Read(hc::settings::Section::General).toObject()["specialFolderIcons"].toBool());
        // Supply namespace entries through a fixture so the real Shell icon
        // path is covered without depending on network discovery or its timing.
        class IconProvider : public GatedProvider {
        public:
            QString custom_;
            hc::storage::Task<std::unique_ptr<hc::storage::IEntryCursor>> Enumerate(hc::storage::Locator, hc::storage::OperationContext) override {
                using namespace hc::storage;
                std::vector<EntrySnapshot> Entries;
                for (const auto* Name : {"virtual", "nested", "shell-local"}) {
                    const auto Id = inner_.Add(inner_.Root(), Name, {}, true);
                    auto Entry = inner_.Stat(Id, {}).get().Value(); Entry.attributes_ = FILE_ATTRIBUTE_DIRECTORY;
                    if (std::string_view(Name) == "virtual") Entry.locator_ = {LocatorKind::Shell, "::{645FF040-5081-101B-9F08-00AA002F954E}"};
                    else if (std::string_view(Name) == "nested") Entry.locator_ = {LocatorKind::Local, custom_.toStdString(), {{"archive", "inner"}}};
                    else { Entry.locator_ = {LocatorKind::Shell, "unused-parsing-name"}; Entry.hints_["filesystemPath"] = custom_.toStdString(); }
                    Entries.push_back(std::move(Entry));
                }
                return Ready<std::unique_ptr<IEntryCursor>>(std::unique_ptr<IEntryCursor>(new test::MemoryCursor(std::move(Entries), std::make_shared<test::FaultState>())));
            }
        };
        auto Provider = std::make_shared<IconProvider>(); Provider->custom_ = Custom;
        Model->SetRoot(nsHC::TySpFileSystem(new GatedFs(Provider))); Model->SetCurrentPath("\\"); Model->Refresh();
        QTRY_VERIFY(!Model->IsLoading() && names(Model).contains("virtual"));
        const auto VirtualBase = Image("virtual"); QVERIFY(!VirtualBase.isNull());
        QTRY_VERIFY_WITH_TIMEOUT(Image("shell-local") != VirtualBase, 10000);
        QCOMPARE(Image("nested"), VirtualBase);
        General = Settings.Read(hc::settings::Section::General).toObject(); General["virtualFolderIcons"] = true;
        QVERIFY(Settings.Put(hc::settings::Section::General, General)); Model->ApplyDisplaySettings();
        QTRY_VERIFY_WITH_TIMEOUT(Image("virtual") != VirtualBase, 10000);
        General["virtualFolderIcons"] = false; QVERIFY(Settings.Put(hc::settings::Section::General, General)); Model->ApplyDisplaySettings();
        QTRY_COMPARE(Image("virtual"), VirtualBase);
    }

    void displayAndNavigationSettingsUi()
    {
        auto& Settings = hc::settings::AppSettings(); const auto Original = Settings.Object();
        const auto Workspace = main_->CaptureWorkspace(); auto* Colors = TyStColorSchemeMgr::GetInstance();
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        const auto Restore = qScopeGuard([&] {
            Settings.DiscardEdit(); Settings.Replace(Original); Settings.Commit();
            emit Colors->sigColorSchemeChanged(Colors->GetColorScheme(Colors->GetCurrentColorScheme()));
            main_->RestoreWorkspace(Workspace);
        });
        QVERIFY(QDir(Directory.path()).mkdir("zzPrevious"));
        for (const auto* Name : {"item2.txt", "item10.txt", "hidden.txt", "a-case.txt", "B-case.txt"}) {
            QFile File(Directory.filePath(Name)); QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write(QByteArray(2048, 'x')), 2048);
        }
        QVERIFY(SetFileAttributesW(QDir::toNativeSeparators(Directory.filePath("hidden.txt")).toStdWString().c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE));
        auto* Panel = panels_.front(); Panel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {});
        auto* Model = static_cast<CFSModel*>(model(Panel));
        auto* View = qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex())); QVERIFY(View);
        QTRY_VERIFY(!Model->IsLoading() && names(Model).contains("item2.txt"));
        // Navigation intentionally retains each tab's search. This scenario
        // must clear the preceding metadata test's name filter before sorting.
        Panel->SetSearch({});
        auto* Proxy = qobject_cast<QAbstractProxyModel*>(View->BaseView()->model());
        const auto Find = [&](const QString& Name) {
            for (int Row = 0; Row < Model->rowCount(); ++Row) if (Model->GetName(Model->index(Row, 0)) == Name) return Model->index(Row, 0);
            return QModelIndex{};
        };
        const auto VisualRow = [&](const QString& Name) { const auto Index = Find(Name); return View->BaseView()->getRow(Proxy ? Proxy->mapFromSource(Index) : Index).rowIndex(); };
        // Establish a name sort independently of the preceding custom-profile scenario.
        QVERIFY(View->ApplyColumnProfile(hc::columns::DefaultProfile()));
        QTest::qWait(180); // Let deferred record/layout work settle before opening the settings draft.
        QTRY_VERIFY(Panel->findChild<QLabel*>("lblVolumeStatus")->text().contains(QStringLiteral("여유")));
        const auto BeforeHeader = Panel->findChild<QLabel*>("lblVolumeStatus")->text();
        QMainOpts Dialog(main_); Dialog.SetListView(View); Dialog.show(); QTest::qWait(50); Dialog.LoadSettings();
        Dialog.findChild<QRadioButton*>("radioButton_4")->setChecked(true);
        Dialog.findChild<QRadioButton*>("radioButton_2")->setChecked(true);
        Dialog.findChild<QCheckBox*>("chkIsDimmedIconWhenHidden")->setChecked(false);
        Dialog.findChild<QCheckBox*>("chkSortNumericAware")->setChecked(false);
        Dialog.findChild<QCheckBox*>("chkSortCaseSensitive")->setChecked(true);
        Dialog.findChild<QCheckBox*>("chkSortLocaleAware")->setChecked(false);
        Dialog.findChild<QComboBox*>("cbxHeaderSizeDisplay")->setCurrentIndex(0);
        Dialog.findChild<QComboBox*>("cbxFooterSizeDisplay")->setCurrentIndex(0);
        auto* Navigation = Dialog.findChild<QCheckBox*>("hc.navigation.focusPreviousDirectory"); QVERIFY(Navigation); Navigation->setChecked(true);
        QVERIFY(Dialog.SaveSettings());
        QCOMPARE(Model->data(Find("zzPrevious")).toString(), QString("[zzPrevious]"));
        QCOMPARE(Model->GetName(Find("zzPrevious")), QString("zzPrevious"));
        QVERIFY(!Model->data(Find("item2.txt"), Qt::DecorationRole).isValid());
        QCOMPARE(Model->data(Find("hidden.txt"), CFsModelT::USR_ROLE_HIDDENOPACITY).toFloat(), 1.0f);
        QTRY_VERIFY(VisualRow("item10.txt") < VisualRow("item2.txt"));
        QTRY_VERIFY(VisualRow("B-case.txt") < VisualRow("a-case.txt"));
        QTRY_VERIFY(Panel->findChild<QLabel*>("lblVolumeStatus")->text().contains(QStringLiteral("여유")));
        QTRY_VERIFY(Panel->findChild<QLabel*>("lblVolumeStatus")->text() != BeforeHeader);
        QVERIFY(Panel->findChild<QLabel*>("lblStatus")->text().contains("10240"));
        Dialog.findChild<QRadioButton*>("radioButton_3")->setChecked(true);
        Dialog.findChild<QRadioButton*>("radioButton_6")->setChecked(true);
        Dialog.findChild<QCheckBox*>("chkSortNumericAware")->setChecked(true);
        Dialog.findChild<QCheckBox*>("chkSortCaseSensitive")->setChecked(false);
        Dialog.findChild<QCheckBox*>("chkIsDimmedIconWhenHidden")->setChecked(true);
        QVERIFY(Dialog.SaveSettings());
        QCOMPARE(Model->data(Find("zzPrevious")).toString(), QString("zzPrevious\\"));
        QCOMPARE(Model->data(Find("hidden.txt"), CFsModelT::USR_ROLE_HIDDENOPACITY).toFloat(), 0.5f);
        QTRY_VERIFY(VisualRow("item2.txt") < VisualRow("item10.txt"));
        QTRY_VERIFY(VisualRow("a-case.txt") < VisualRow("B-case.txt"));
        QTRY_VERIFY(!qvariant_cast<QPixmap>(Model->data(Find("item2.txt"), Qt::DecorationRole)).isNull());
        Dialog.reject();
        Settings.Reload();
        QCOMPARE(Settings.Read(hc::settings::Section::General).toObject()["directoryStyle"].toString(), QString("suffix"));
        {
            QMainOpts Reopen(main_); Reopen.show(); QTest::qWait(50); Reopen.LoadSettings();
            QVERIFY(Reopen.findChild<QRadioButton*>("radioButton_6")->isChecked());
            QVERIFY(Reopen.findChild<QRadioButton*>("radioButton_3")->isChecked());
            auto* Categories = Reopen.findChild<QListWidget*>("lstOptCate");
            const auto Pages = Categories->findItems(QStringLiteral("탐색"), Qt::MatchExactly); QCOMPARE(Pages.size(), 1);
            Categories->setCurrentItem(Pages.front());
            QVERIFY(Reopen.findChild<QCheckBox*>("hc.navigation.focusPreviousDirectory")->isVisible());
            QVERIFY(Reopen.grab().save(QCoreApplication::applicationDirPath() + "/navigation-settings.png"));
            Reopen.findChild<QRadioButton*>("radioButton_4")->setChecked(true); Reopen.reject();
        }
        QCOMPARE(Settings.Read(hc::settings::Section::General).toObject()["iconMode"].toString(), QString("associated"));
        for (const bool Enabled : {true, false}) {
            auto General = Settings.Read(hc::settings::Section::General).toObject(); General["focusPreviousDirectory"] = Enabled;
            QVERIFY(Settings.Put(hc::settings::Section::General, General));
            for (auto* Target : panels_) {
                Target->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {});
                auto* TargetModel = static_cast<CFSModel*>(model(Target));
                auto* TargetView = qobject_cast<CGridView*>(Target->GetFocusView(Target->CurrentTabIndex()));
                QTRY_VERIFY(!TargetModel->IsLoading() && names(TargetModel).contains("zzPrevious"));
                TargetModel->ChangeDirectory("zzPrevious"); QTRY_VERIFY(!TargetModel->IsLoading());
                TargetModel->ChangeDirectory(".."); QTRY_VERIFY(!TargetModel->IsLoading() && names(TargetModel).contains("zzPrevious"));
                auto Index = TargetView->BaseView()->focusedRow().modelIndex(0);
                if (auto* TargetProxy = qobject_cast<QAbstractProxyModel*>(TargetView->BaseView()->model())) Index = TargetProxy->mapToSource(Index);
                QCOMPARE(TargetModel->GetName(Index), Enabled ? QString("zzPrevious") : QString(".."));
            }
        }
    }
    void archiveNavigationPositionUi_data()
    {
        QTest::addColumn<QString>("Mode"); QTest::addColumn<bool>("Viewport"); QTest::addColumn<bool>("FocusPrevious"); QTest::addColumn<int>("NativeKey");
        for (const auto& Mode : {QString("one-line"), QString("two-line")}) for (bool Viewport : {false, true}) for (bool FocusPrevious : {true, false})
            QTest::newRow(qPrintable(Mode + (Viewport ? "-viewport" : "-grid") + (FocusPrevious ? "-restore" : "-top"))) << Mode << Viewport << FocusPrevious << 0;
        for (const auto& Mode : {QString("one-line"), QString("two-line")}) for (int NativeKey : {1, 2})
            QTest::newRow(qPrintable(Mode + (NativeKey == 1 ? "-native-navigation" : "-native-keypad"))) << Mode << false << true << NativeKey;
    }
    void archiveNavigationPositionUi()
    {
        QFETCH(QString, Mode); QFETCH(bool, Viewport); QFETCH(bool, FocusPrevious); QFETCH(int, NativeKey);
        main_->resize(1400, 900); main_->show();
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        auto& Settings = hc::settings::AppSettings(); const auto OriginalGeneral = Settings.Read(hc::settings::Section::General).toObject();
        const auto Workspace = main_->CaptureWorkspace();
        const auto Restore = qScopeGuard([&] { Settings.Put(hc::settings::Section::General, OriginalGeneral); main_->RestoreWorkspace(Workspace); QThreadPool::globalInstance()->waitForDone(); });
        auto General = OriginalGeneral; General["focusPreviousDirectory"] = FocusPrevious; QVERIFY(Settings.Put(hc::settings::Section::General, General));
        for (int I = 0; I < 200; ++I) {
            QFile File(Directory.filePath(QString("file%1.txt").arg(I, 3, 10, QChar('0')))); QVERIFY(File.open(QIODevice::WriteOnly)); QVERIFY(File.write("payload") > 0);
        }
        const auto Name = QString("file100.zip"); const auto ArchivePath = Directory.filePath(Name);
        hc::plugins::ArchiveUpdate Create{ArchivePath.toStdString(), Directory.path().toStdString(), {"file000.txt"}};
        Create.creation_ = hc::plugins::ArchiveCreationOptions{1, false};
        auto Made = hc::plugins::MakeApprovedWcxBackend(hc::plugins::BuiltinSevenZipOptions())->Update(Create, {});
        QVERIFY2(Made.HasValue(), Made.HasValue() ? "" : Made.Failure().code_.c_str());
        auto* Panel = panels_.front(); auto* Model = static_cast<CFSModel*>(model(Panel));
        auto* View = qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex())); QVERIFY(View);
        Model->SetProviderEnabled(true); Panel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {});
        QTRY_VERIFY(!Model->IsLoading()); QVERIFY(View->SetListMode(Mode));
        const auto Find = [&](const QString& Wanted) { for (int Row = 0; Row < Model->rowCount(); ++Row) if (Model->GetName(Model->index(Row, 0)) == Wanted) return Model->index(Row, 0); return QModelIndex{}; };
        const auto Entry = Model->GetFileInfo(Find(Name)); QVERIFY(Entry && Entry->storageEntry_);
        const auto Focus = hc::workspace::SelectionKey(*Entry->storageEntry_);
        View->RestoreSelection({}, Focus); QTest::qWait(80);
        auto* Native = View->BaseView(); auto* Grid = View->findChild<Qtitan::Grid*>(); QVERIFY(Grid);
        Native->verticalScrollBar()->setValue(Native->verticalScrollBar()->maximum() / 2); QTest::qWait(80);
        const auto Before = View->CaptureScrollState(); QVERIFY(Before["y"].toInt() > 0);
        QVERIFY(Panel->grab().save(QCoreApplication::applicationDirPath() + "/archive-before-" + QTest::currentDataTag() + ".png"));
        auto* Busy = Panel->findChild<fm::ui::ProgressBar*>("hc.navigation.progress"); QVERIFY(Busy);
        bool ArchiveProgress = false;
        connect(Model, &CFSModel::DirectoryReadStarted, View, [&](const QString&, bool Archive) {
            if (Archive) ArchiveProgress = !Busy->isHidden() && Busy->minimum() == 0 && Busy->maximum() == 0;
        });
        Panel->sig_NotifyPanelActivated(); QApplication::setActiveWindow(main_); Panel->SetFocusView(Panel->CurrentTabIndex());
        if (NativeKey) {
            const auto Window = reinterpret_cast<HWND>(main_->winId());
            const bool Topmost = GetWindowLongPtrW(Window, GWL_EXSTYLE) & WS_EX_TOPMOST;
            SetWindowPos(Window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            RECT Rect{}; GetWindowRect(Window, &Rect); POINT Cursor{}; GetCursorPos(&Cursor);
            SetCursorPos(Rect.left + 100, Rect.top + 12);
            INPUT Click[2]{}; Click[0].type = Click[1].type = INPUT_MOUSE; Click[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN; Click[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
            QCOMPARE(SendInput(2, Click, sizeof(INPUT)), UINT(2)); QTest::qWait(50); SetCursorPos(Cursor.x, Cursor.y);
            if (!Topmost) SetWindowPos(Window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            QTRY_COMPARE(GetForegroundWindow(), Window);
            Panel->SetFocusView(Panel->CurrentTabIndex());
            QTRY_VERIFY(QApplication::focusWidget() == Grid || Grid->isAncestorOf(QApplication::focusWidget()));
            qInfo() << "Native archive input focus" << QApplication::focusWidget();
        }
        QWidget* Target = Viewport ? Grid->viewport() : Grid;
        for (int Cycle = 0; Cycle < 2; ++Cycle) {
            View->RestoreSelection({}, Focus); View->RestoreScrollState(Before);
            if (NativeKey) {
                INPUT Keys[4]{};
                for (auto& Key : Keys) Key.type = INPUT_KEYBOARD;
                Keys[0].ki.wVk = Keys[3].ki.wVk = VK_CONTROL; Keys[3].ki.dwFlags = KEYEVENTF_KEYUP;
                Keys[1].ki.wVk = Keys[2].ki.wVk = VK_NEXT;
                Keys[1].ki.dwFlags = NativeKey == 1 ? KEYEVENTF_EXTENDEDKEY : 0; Keys[2].ki.dwFlags = Keys[1].ki.dwFlags | KEYEVENTF_KEYUP;
                QCOMPARE(SendInput(4, Keys, sizeof(INPUT)), UINT(4));
            } else QTest::keyClick(Target, Qt::Key_PageDown, Qt::ControlModifier);
            QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading() && ((Model->GetRoot()->GetFeatures() & nsHC::FS_FEA_PACK) || Model->LastNavigationError()), 15000);
            QVERIFY2(!Model->LastNavigationError(), Model->LastNavigationError() ? Model->LastNavigationError()->code_.c_str() : "");
            QVERIFY(Model->GetRoot()->GetFeatures() & nsHC::FS_FEA_PACK); QVERIFY(ArchiveProgress); QVERIFY(Busy->isHidden());
            QVERIFY(Find("file000.txt").isValid());
            QVERIFY(TyStCommandMgr::GetInstance()->Bridge()->ExecuteCommand("hc.navigate.parent"));
            QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading() && !(Model->GetRoot()->GetFeatures() & nsHC::FS_FEA_PACK), 15000);
            if (FocusPrevious) QTRY_COMPARE(View->CaptureFocus(), Focus);
            else QTRY_COMPARE(Native->focusedRowIndex(), 0);
            QTest::qWait(100);
            QVERIFY(Panel->grab().save(QCoreApplication::applicationDirPath() + "/archive-after-" + QTest::currentDataTag() + ".png"));
            if (FocusPrevious) QCOMPARE(View->CaptureScrollState(), Before);
            else QCOMPARE(Native->verticalScrollBar()->value(), 0);
        }
    }
    void navigationProgressUi_data()
    {
        QTest::addColumn<QString>("Outcome");
        for (const auto& Outcome : {QString("complete"), QString("cancel"), QString("failure")}) QTest::newRow(qPrintable(Outcome)) << Outcome;
    }
    void navigationProgressUi()
    {
        QFETCH(QString, Outcome);
        main_->resize(1400, 900); main_->show();
        const auto Workspace = main_->CaptureWorkspace();
        const auto Restore = qScopeGuard([&] { main_->RestoreWorkspace(Workspace); QThreadPool::globalInstance()->waitForDone(); });
        auto Provider = std::make_shared<GatedProvider>(); Provider->fail_ = Outcome == "failure";
        bool Released = false; const auto Release = qScopeGuard([&] { if (!Released) Provider->release_.set_value(); });
        auto* Panel = panels_.front(); auto* Model = static_cast<CFSModel*>(model(Panel));
        auto* Busy = Panel->findChild<fm::ui::ProgressBar*>("hc.navigation.progress"); QVERIFY(Busy);
        auto* Path = Panel->findChild<fm::ui::BreadcrumbBar*>("hc.panelPath"); QVERIFY(Path);
        const auto Size = Panel->size();
        Model->SetRoot(nsHC::TySpFileSystem(new GatedFs(Provider))); Model->SetCurrentPath("\\"); Model->Refresh();
        QTRY_VERIFY(Provider->started_.load()); QVERIFY(Model->IsLoading()); QVERIFY(!Busy->isHidden());
        QCOMPARE(Busy->minimum(), 0); QCOMPARE(Busy->maximum(), 0); QCOMPARE(Busy->focusPolicy(), Qt::NoFocus);
        QVERIFY(Busy->testAttribute(Qt::WA_TransparentForMouseEvents)); QCOMPARE(Panel->size(), Size);
        QVERIFY(Path->rect().contains(Busy->geometry()));
        const auto Frame = Busy->grab().toImage(); QTest::qWait(150); QVERIFY(Frame != Busy->grab().toImage());
        QVERIFY(Panel->grab(QRect(0, 0, Panel->width(), qMin(150, Panel->height()))).save(QCoreApplication::applicationDirPath() + "/navigation-progress-" + Outcome + ".png"));
        bool Responsive = false; QTimer::singleShot(0, Panel, [&] { Responsive = true; }); QTRY_VERIFY(Responsive);
        const int OldIndex = Panel->CurrentTabIndex(); Panel->AddTab(false); QVERIFY(Busy->isHidden());
        Panel->findChild<QTabWidget*>("tabWidget")->setCurrentIndex(OldIndex); QTRY_VERIFY(!Busy->isHidden());
        if (Outcome == "cancel") { Model->CancelNavigation(); QVERIFY(Busy->isHidden()); }
        Provider->release_.set_value(); Released = true;
        QTRY_VERIFY(!Model->IsLoading()); QVERIFY(Busy->isHidden());
        if (Outcome == "failure") {
            QVERIFY(Model->LastNavigationError()); QTRY_VERIFY(Panel->findChild<QMessageBox*>("hc.navigationError"));
            Panel->findChild<QMessageBox*>("hc.navigationError")->button(QMessageBox::Cancel)->click();
        } else if (Outcome == "complete") QVERIFY(!Model->LastNavigationError());
    }
    void navigationEnterRepeatUi()
    {
        // Holding Enter alternates between a folder and its parent faster than
        // listings complete. Every listing must either publish or be superseded;
        // no navigation error may surface.
        main_->resize(1400, 900); main_->show();
        const auto Workspace = main_->CaptureWorkspace();
        const auto Restore = qScopeGuard([&] { main_->RestoreWorkspace(Workspace); });
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        auto Root = qEnvironmentVariable("HC_PROBE_ROOT"); const auto Child = qEnvironmentVariable("HC_PROBE_CHILD", "Dev");
        if (Root.isEmpty()) {
            Root = Directory.path(); QVERIFY(QDir(Root).mkdir(Child));
            for (int I = 0; I < 40; ++I) { QFile F(QDir(Root).filePath(Child + QString("/item_%1.txt").arg(I))); QVERIFY(F.open(QIODevice::WriteOnly)); F.write("d"); }
            for (int I = 0; I < 8; ++I) { QFile F(QDir(Root).filePath(QString("root_%1.txt").arg(I))); QVERIFY(F.open(QIODevice::WriteOnly)); F.write("d"); }
        }
        auto* Panel = panels_.front(); auto* Model = static_cast<CFSModel*>(model(Panel));
        auto* View = static_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex())); auto* Grid = View->findChild<Qtitan::Grid*>(); QVERIFY(Grid);
        View->SetListMode("one-line");
        Panel->ChangeCurrentPath(QDir::toNativeSeparators(Root), {}); QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading() && names(Model).contains(Child), 30000);
        // Superseded listings report "cancelled" to automation only; the panel
        // must never enter an error state, show its notice, or post a status
        // bar message while the user keeps navigating.
        QStringList Failures; int Dialogs = 0, Notices = 0, Superseded = 0;
        auto* Retry = Panel->findChild<QPushButton*>("hc.workspaceRetry"); QVERIFY(Retry);
        const auto Failed = connect(Model, &CFsModelT::sigNavigationFailed, this, [&](const QString& Code) {
            if (const auto Error = Model->LastNavigationError()) Failures.append(Code + QString("/%1@").arg(int(Error->category_)) + Model->GetCurrentPathWithRoot());
            else ++Superseded;
        });
        const auto Completed = connect(Model, &CFsModelT::sigRefreshCompleted, this, [&] {
            if (const auto Error = Model->LastNavigationError()) Failures.append(QString::fromStdString(Error->code_) + "/completed@" + Model->GetCurrentPathWithRoot());
        });
        const auto Shown = connect(Model, &CFSModel::DirectoryReadFailed, this, [&](const QString& Path, bool) { ++Dialogs; qWarning() << "DirectoryReadFailed" << Path; });
        const auto Disconnect = qScopeGuard([&] { disconnect(Failed); disconnect(Completed); disconnect(Shown); });
        const auto ChildIndex = [&] { for (int Row = 0; Row < Model->rowCount(); ++Row) if (Model->GetName(Model->index(Row, 0)) == Child) return Model->index(Row, 0); return QModelIndex{}; }();
        QVERIFY(ChildIndex.isValid());
        const auto ChildKey = hc::workspace::SelectionKey(*Model->GetFileInfo(ChildIndex)->storageEntry_);
        View->RestoreSelection({}, ChildKey); Panel->sig_NotifyPanelActivated(); View->EnsureKeyboardFocusOnView();
        QCOMPARE(View->CaptureFocus(), ChildKey);
        int Transitions = 0; auto LastPath = Model->GetCurrentPathWithRoot();
        for (int I = 0; I < 400; ++I) {
            QTest::keyClick(Grid, Qt::Key_Return);
            QTest::qWait(3 + (I % 9) * 4);
            if (auto* Dialog = Panel->findChild<QMessageBox*>("hc.navigationError")) Dialog->button(QMessageBox::Cancel)->click();
            if (Retry->isVisible() || main_->statusBar()->currentMessage().contains("목록")) ++Notices;
            const auto Path = Model->GetCurrentPathWithRoot(); if (Path != LastPath) { ++Transitions; LastPath = Path; }
        }
        QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 30000);
        qWarning() << "navigationEnterRepeatUi transitions" << Transitions << "superseded" << Superseded << "failures" << Failures << "dialogs" << Dialogs << "notices" << Notices;
        QVERIFY2(Transitions >= 20, qPrintable(QString("only %1 transitions").arg(Transitions)));
        QVERIFY2(Dialogs == 0 && Failures.isEmpty(), qPrintable(Failures.join(", ")));
        QCOMPARE(Notices, 0);
        QVERIFY(!Model->LastNavigationError());
        // An explicit cancel still settles the panel in a visible cancelled state.
        // Refresh dispatches synchronously, unlike the queued Enter command.
        Model->Refresh(); QVERIFY(Model->IsLoading()); Model->CancelNavigation();
        QVERIFY(!Model->IsLoading()); QVERIFY(Model->LastNavigationError()); QCOMPARE(Model->LastNavigationError()->category_, hc::storage::ErrorCategory::Cancelled);
        QTRY_VERIFY(Retry->isVisible());
        Model->DismissNavigationError(); QTRY_VERIFY(!Retry->isVisible());
    }
    void navigationRefreshSettingsUi()
    {
        auto& Settings = hc::settings::AppSettings(); const auto Original = Settings.Object();
        const auto Workspace = main_->CaptureWorkspace();
        const auto Restore = qScopeGuard([&] {
            Settings.DiscardEdit(); Settings.Replace(Original); Settings.Commit(); main_->RestoreWorkspace(Workspace);
            for (auto* Model : main_->findChildren<CFSModel*>()) Model->ApplyRefreshSettings();
        });
        {
            QMainOpts Dialog(main_); Dialog.show(); QTest::qWait(80); Dialog.LoadSettings();
            auto* Categories = Dialog.findChild<QListWidget*>("lstOptCate"); QVERIFY(Categories);
            const auto Navigation = Categories->findItems(QStringLiteral("탐색"), Qt::MatchExactly); QCOMPARE(Navigation.size(), 1);
            Categories->setCurrentItem(Navigation.front());
            auto* Tabs = Dialog.findChild<QTabWidget*>("hc.navigation.tabs"); QVERIFY(Tabs); QCOMPARE(Tabs->count(), 3);
            QCOMPARE(Tabs->tabText(1), QStringLiteral("빠른 찾기"));
            QCOMPARE(Tabs->tabText(2), QStringLiteral("새로고침")); Tabs->setCurrentIndex(2);
            auto* Poll = Dialog.findChild<QCheckBox*>("hc.navigation.refresh.fallbackEnabled"); QVERIFY(Poll);
            auto* Seconds = Dialog.findChild<QSpinBox*>("hc.navigation.refresh.fallbackSeconds"); QVERIFY(Seconds);
            QVERIFY(!Poll->isChecked()); QVERIFY(!Seconds->isEnabled()); QCOMPARE(Seconds->value(), 15);
            Poll->setChecked(true); QVERIFY(Seconds->isEnabled()); Seconds->setValue(37);
            QVERIFY(Dialog.SaveSettings());
            QCOMPARE(Settings.CommittedObject()["general"].toObject()["refresh"].toObject()["fallbackSeconds"].toInt(), 37);
            Poll->setChecked(false); Seconds->setValue(99); Dialog.reject();
        }
        {
            QMainOpts Dialog(main_); Dialog.show(); QTest::qWait(50); Dialog.LoadSettings();
            QVERIFY(Dialog.findChild<QCheckBox*>("hc.navigation.refresh.fallbackEnabled")->isChecked());
            QCOMPARE(Dialog.findChild<QSpinBox*>("hc.navigation.refresh.fallbackSeconds")->value(), 37);
            auto* Categories = Dialog.findChild<QListWidget*>("lstOptCate");
            Categories->setCurrentItem(Categories->findItems(QStringLiteral("탐색"), Qt::MatchExactly).front());
            Dialog.findChild<QTabWidget*>("hc.navigation.tabs")->setCurrentIndex(2); QTest::qWait(80);
            QVERIFY(Dialog.grab().save(QCoreApplication::applicationDirPath() + "/navigation-refresh-settings.png"));
            Dialog.reject();
        }
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        const auto Create = [&](const QString& Name) {
            QFile File(Directory.filePath(Name)); return File.open(QIODevice::WriteOnly) && File.write("refresh") == 7;
        };
        QVERIFY(Create("b.txt")); QVERIFY(Create("z.txt"));
        auto General = Settings.Read(hc::settings::Section::General).toObject();
        hc::refresh::Settings Policy; Policy.pauseInBackground = false; Policy.appendNewEntries = true;
        Policy.updateHeader = Policy.updateFooter = false; General["refresh"] = Policy.ToJson();
        QVERIFY(Settings.Put(hc::settings::Section::General, General));
        auto* Panel = panels_.front(); auto* Model = static_cast<CFSModel*>(model(Panel));
        Model->ApplyRefreshSettings(); Panel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {});
        QTRY_VERIFY(!Model->IsLoading() && names(Model).contains("z.txt"));
        auto* View = qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex())); QVERIFY(View);
        const auto VisibleNames = [&] {
            QStringList Result;
            for (int Row = 0; Row < View->BaseView()->getRowCount(); ++Row) {
                auto Index = View->BaseView()->getRow(Row).modelIndex(0);
                if (auto* Proxy = qobject_cast<QAbstractProxyModel*>(View->BaseView()->model())) Index = Proxy->mapToSource(Index);
                const auto Name = Model->GetName(Index); if (Name != "..") Result.append(Name);
            }
            return Result;
        };
        QTRY_COMPARE(VisibleNames(), QStringList({"b.txt", "z.txt"}));
        const auto Footer = Panel->findChild<QLabel*>("lblStatus")->text();
        const auto Header = Panel->findChild<QLabel*>("lblVolumeStatus")->text();
        QVERIFY(Create("a.txt")); QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading() && names(Model).contains("a.txt"), 5000);
        QTRY_COMPARE(VisibleNames(), QStringList({"b.txt", "z.txt", "a.txt"}));
        QCOMPARE(Panel->findChild<QLabel*>("lblStatus")->text(), Footer);
        QCOMPARE(Panel->findChild<QLabel*>("lblVolumeStatus")->text(), Header);
        Panel->RefreshSource(Panel->CurrentTabIndex()); QTRY_VERIFY(!Model->IsLoading());
        QTRY_COMPARE(VisibleNames(), QStringList({"a.txt", "b.txt", "z.txt"}));
        QVERIFY(Panel->findChild<QLabel*>("lblStatus")->text() != Footer);
        const auto FileSize = [&] {
            for (int Row = 0; Row < Model->rowCount(); ++Row) {
                const auto Item = Model->GetFileInfo(Model->index(Row, 0));
                if (Item && Item->Name_ == "b.txt") return Item->Size_;
            }
            return qint64(-1);
        };
        { QFile File(Directory.filePath("b.txt")); QVERIFY(File.open(QIODevice::Append)); QCOMPARE(File.write("size"), 4); }
        QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading() && FileSize() == 11, 5000);
        Policy.excludedDrives = QDir::fromNativeSeparators(Directory.path()).left(2); General["refresh"] = Policy.ToJson();
        QVERIFY(Settings.Put(hc::settings::Section::General, General)); Model->ApplyRefreshSettings();
        QVERIFY(Create("excluded.txt")); QTest::qWait(600); QVERIFY(!names(Model).contains("excluded.txt"));
        Panel->RefreshSource(Panel->CurrentTabIndex()); QTRY_VERIFY(!Model->IsLoading() && names(Model).contains("excluded.txt"));
    }
    void directoryHotListPlacementUi()
    {
        const auto OldCursor = QCursor::pos();
        const auto Restore = qScopeGuard([&] { QCursor::setPos(OldCursor); });
        for (auto* Panel : panels_)
        {
            auto* View = qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex())); QVERIFY(View);
            auto* Grid = View->findChild<Qtitan::Grid*>(); QVERIFY(Grid);
            Panel->sig_NotifyPanelActivated();
            QPoint Previous;
            for (int Attempt = 0; Attempt < 2; ++Attempt)
            {
                main_->activateWindow();
                QCursor::setPos(main_->mapToGlobal(Attempt ? main_->rect().bottomRight() : QPoint(0, 0)));
                QTest::qWait(100); // Drain native mouse/activation events before opening the keyboard menu.
                QApplication::setActiveWindow(main_);
                Panel->sig_NotifyPanelActivated();
                bool Seen = false;
                QTimer::singleShot(150, main_, [&] {
                    auto* Menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                    if (!Menu) return;
                    const auto Close = qScopeGuard([&] { Menu->close(); });
                    QCOMPARE(Menu->objectName(), QString("hc.directoryHotList"));
                    QRect Header;
                    for (const auto& Hit : Grid->hitInfoAll())
                        if (Hit.info() == Qtitan::GridHitInfo::Column) Header = Header.united(Hit.rect());
                    QVERIFY(!Header.isEmpty());
                    const auto Expected = Grid->viewport()->mapToGlobal(QPoint(0, Header.bottom() + 1));
                    QVERIFY(qAbs(Menu->pos().x() - Expected.x()) <= 2);
                    QVERIFY(qAbs(Menu->pos().y() - Expected.y()) <= 2);
                    QVERIFY(View->screen()->availableGeometry().contains(Menu->geometry()));
                    QVERIFY(Menu->activeAction() && !Menu->activeAction()->isSeparator());
                    if (Attempt) QCOMPARE(Menu->pos(), Previous);
                    Previous = Menu->pos();
                    Seen = true;
                    QVERIFY(Menu->grab().save(QCoreApplication::applicationDirPath()
                        + (Panel == panels_.front() ? "/favorites-left-menu.png" : "/favorites-right-menu.png")));
                    QVERIFY(main_->grab().save(QCoreApplication::applicationDirPath()
                        + (Panel == panels_.front() ? "/favorites-left.png" : "/favorites-right.png")));
                });
                QVERIFY(TyStCommandMgr::GetInstance()->Bridge()->ExecuteCommand(
                    Attempt ? "cm_DirectoryHotList" : "hc.navigate.favorites"));
                QTRY_VERIFY(Seen);
            }
        }
    }
    void comboAndListPreferencesUi()
    {
        auto& Settings = hc::settings::AppSettings(); const auto Original = Settings.Object();
        auto* Colors = TyStColorSchemeMgr::GetInstance(); const auto Workspace = main_->CaptureWorkspace();
        const auto Restore = qScopeGuard([&] {
            Settings.DiscardEdit(); Settings.Replace(Original); Settings.Commit(); Colors->Refresh();
            emit Colors->sigColorSchemeChanged(Colors->GetColorScheme(Colors->GetCurrentColorScheme()));
            main_->RestoreWorkspace(Workspace);
        });
        auto* Panel = panels_.front(); auto* View = qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex())); QVERIFY(View);
        auto* Adapter = View->ColumnWidths(); QVERIFY(Adapter);
        auto* Drives = Panel->findChild<QComboBox*>("cbxVolume"); QVERIFY(Drives);
        QVERIFY(!Panel->findChild<QPushButton*>("btnGridStyle")->isVisible());
        QVERIFY(!main_->findChild<QAction*>("hc.toolbar.hc.view.columns"));
        const auto Before = Adapter->WorkspaceLayout();
        for (const bool Dark : {false, true}) {
            auto Scheme = Colors->GetColorScheme(Dark ? QStringLiteral("기본 다크") : QStringLiteral("기본 라이트"));
            Scheme.Dialog_Font.setPointSize(Dark ? 18 : 11);
            emit Colors->sigColorSchemeChanged(Scheme);
            main_->activateWindow(); QTest::qWait(150);
            Drives->showPopup();
            QTRY_VERIFY(Drives->view()->isVisible());
            QTest::qWait(150);
            for (int Row = 0; Row < Drives->count(); ++Row) {
                const auto Index = Drives->model()->index(Row, 0);
                QVERIFY(!Drives->view()->indexWidget(Index)); // text is drawn once by the delegate
                QVERIFY(Drives->view()->visualRect(Index).height() >= Drives->fontMetrics().height());
            }
            QVERIFY(Drives->view()->window()->grab().save(QCoreApplication::applicationDirPath() + (Dark ? "/drive-popup-dark.png" : "/drive-popup-light.png")));
            Drives->hidePopup();
            QMainOpts Dialog(main_); Dialog.SetListView(View); Dialog.ShowListSettingsPage(); Dialog.show();
            QCoreApplication::processEvents();
            QVERIFY(Dialog.grab().save(QCoreApplication::applicationDirPath() + (Dark ? "/list-preferences-dark.png" : "/list-preferences-light.png")));
            auto* Editor = Dialog.findChild<QtColumnProfileEditor*>(); QVERIFY(Editor);
            QVERIFY(!Dialog.findChild<QDialog*>("hc.columnWidthsDialog"));
            Editor->findChild<QComboBox*>("hc.columns.layout")->setCurrentIndex(1);
            Editor->findChild<QComboBox*>("hc.columns.scope")->setCurrentIndex(1);
            Editor->findChild<QTabWidget*>("hc.columns.pages")->setCurrentIndex(0);
            auto* Choice = Editor->findChild<QComboBox*>("hc.columns.widthMode");
            Dialog.activateWindow(); QTest::qWait(150); QVERIFY(Choice->isVisible());
            for (int Selected = 0; Selected < Choice->count(); ++Selected) {
                Choice->setCurrentIndex(Selected); Choice->showPopup();
                QTRY_VERIFY_WITH_TIMEOUT(Choice->view()->isVisible(), 3000);
                QTest::qWait(100);
                for (int Row = 0; Row < Choice->count(); ++Row)
                    QVERIFY(Choice->view()->viewport()->rect().contains(Choice->view()->visualRect(Choice->model()->index(Row, 0))));
                Choice->hidePopup();
            }
            QVERIFY(Dialog.grab().save(QCoreApplication::applicationDirPath() + (Dark ? "/column-editor-dark.png" : "/column-editor-light.png")));
            QCOMPARE(Adapter->WorkspaceLayout(), Before);
            if (Dark) {
                Dialog.findChild<QPushButton*>("btnApply")->click();
                QCOMPARE(Adapter->LayoutMode(), QString("name-first"));
                const auto Applied = Adapter->WorkspaceLayout();
                Dialog.reject(); QCOMPARE(Adapter->WorkspaceLayout(), Applied);
            } else {
                Dialog.reject(); QCOMPARE(Adapter->WorkspaceLayout(), Before); QCOMPARE(Settings.Object(), Original);
            }
        }
        // Replace only the network provider with an in-memory fixture. Drive
        // activation itself uses the real product signal and navigation path.
        class NetworkFs : public nsHC::CFileSystemT {
        public:
            NetworkFs() : CFileSystemT("\\") { cate_ = nsHC::FS_CATE_REMOTE; }
            std::shared_ptr<hc::storage::test::MemoryProvider> provider_ = std::make_shared<hc::storage::test::MemoryProvider>();
            mutable QString requested_;
            std::shared_ptr<hc::storage::IStorageProvider> ReadProvider() override { return provider_; }
            hc::storage::Locator ReadLocation(const QString& Path) const override { requested_ = QString::fromStdString(CFileSystemT::ReadLocation(Path).root_); return provider_->Root(); }
        };
        const int NetworkIndex = Drives->findText(tr("네트워크 환경")); QVERIFY(NetworkIndex >= 0);
        const auto OriginalNetwork = Drives->itemData(NetworkIndex);
        const auto RestoreNetwork = qScopeGuard([&] { Drives->setItemData(NetworkIndex, OriginalNetwork); });
        auto Network = QSharedPointer<NetworkFs>::create();
        Drives->setItemData(NetworkIndex, QVariant::fromValue(nsHC::TySpFileSystem(Network)));
        for (int Attempt = 0; Attempt < 2; ++Attempt) {
            { QSignalBlocker Blocker(Drives); Drives->setCurrentIndex(NetworkIndex); }
            QSignalSpy Activated(Drives, &QComboBox::activated);
            QSignalSpy Completed(model(Panel), &CFsModelT::sigRefreshCompleted);
            main_->activateWindow(); QTest::qWait(150);
            Drives->showPopup(); QTRY_VERIFY(Drives->view()->isVisible()); QTest::qWait(150);
            const auto Index = Drives->model()->index(NetworkIndex, 0); Drives->view()->scrollTo(Index);
            Drives->view()->setCurrentIndex(Index);
            QTest::keyClick(Drives->view(), Qt::Key_Return);
            QCOMPARE(Activated.count(), 1); QTRY_VERIFY(Completed.count() > 0);
            QCOMPARE(Network->requested_, QString("\\"));
        }
        if (qEnvironmentVariableIsSet("HC_TEST_NETWORK_DISCOVERY")) {
            const auto Native = nsHC::TySpFileSystem(nsHC::CFileSystemT::MakeFileSystem(R"(\\)"));
            QVERIFY(Native);
            hc::storage::OperationContext Context;
            Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            const auto Location = Native->ReadLocation(R"(\\)");
            auto Task = Native->ReadProvider()->Enumerate(Location, Context);
            QTRY_VERIFY_WITH_TIMEOUT(Task.wait_for(std::chrono::seconds(0)) == std::future_status::ready, 20000);
            auto Cursor = Task.get(); QVERIFY2(Cursor.HasValue(), Cursor.HasValue() ? "" : Cursor.Failure().code_.c_str());
            auto Page = Cursor.Value()->Next(256, Context).get(); QVERIFY(Page.HasValue());
            qInfo() << "Live network discovery entries:" << Page.Value().entries_.size();
            for (const auto& Entry : Page.Value().entries_) qInfo() << QString::fromStdString(Entry.name_);
        }
    }

    void watercolorSettingsUi()
    {
        using namespace hc::settings;
        using namespace fm::style;
        auto& Settings = AppSettings();
        const auto Original = Settings.Object();
        auto* Colors = TyStColorSchemeMgr::GetInstance();
        const auto Restore = qScopeGuard([&] {
            Settings.DiscardEdit(); Settings.Replace(Original); Settings.Commit(); Colors->Refresh();
            emit Colors->sigColorSchemeChanged(Colors->GetColorScheme(Colors->GetCurrentColorScheme()));
        });
        auto* Proxy = qobject_cast<QProxyStyle*>(qApp->style());
        QVERIFY(Proxy);
        QPointer<QStyle> Style(qApp->style()), Base(Proxy->baseStyle());
        QVERIFY(qobject_cast<WatercolorStyle*>(Base.data()));
        QCOMPARE(Base->parent(), Style.data());
        QVERIFY(Settings.Put(Section::Appearance, BuiltInAppearance()));
        Colors->Refresh();
        for (const bool Dark : {false, true}) {
            const auto Name = Dark ? QStringLiteral("기본 다크") : QStringLiteral("기본 라이트");
            auto Scheme = Colors->GetColorScheme(Name);
            const ThemeColors Defaults(Dark ? Variant::Dark : Variant::Light, Design::Watercolor);
            QVERIFY(Scheme.explicitColors_.isEmpty());
            QCOMPARE(Scheme.Dialog_BGColor, Defaults[Token::Win]);
            QCOMPARE(Scheme.FileList_BGColor, Defaults[Token::Surface]);
            // Saving unrelated font changes must not freeze resolved theme colors.
            Scheme.Dialog_Font.setPointSize(17);
            Colors->UpsertColorScheme(Scheme, true);
            QVERIFY(Settings.Commit());
            Colors->Refresh();
            Scheme = Colors->GetColorScheme(Name);
            QVERIFY(Scheme.explicitColors_.isEmpty());
            QCOMPARE(Scheme.Dialog_Font.pointSize(), 17);
            emit Colors->sigColorSchemeChanged(Scheme);
            QCOMPARE(ThemeManager::instance().design(), Design::Watercolor);
            QCOMPARE(ThemeManager::instance().effectiveVariant(), Dark ? Variant::Dark : Variant::Light);
            QCOMPARE(QApplication::palette().color(QPalette::Window), Defaults[Token::Win]);
        }
        {
            QMainOpts Dialog(main_); Dialog.show(); QCoreApplication::processEvents();
            Dialog.findChild<QComboBox*>("hc.settings.theme")->setCurrentIndex(1);
            QVERIFY(Dialog.SaveSettings());
            QVERIFY(Dialog.SaveSettings());
            const auto Changed = Colors->GetColorScheme(QStringLiteral("기본 다크"));
            QVERIFY(Changed.IsDarkMode);
            QVERIFY(Changed.explicitColors_.isEmpty());
            Dialog.accept();
        }
        // Legacy custom colors migrate, including an explicit color equal to the
        // old default when the file already records the user's override intent.
        auto Appearance = BuiltInAppearance();
        auto Schemes = Appearance["schemes"].toObject();
        const auto Name = QStringLiteral("기본 라이트");
        auto Record = Schemes[Name].toObject();
        auto Values = Record["colors"].toObject();
        Values["dialogBackground"] = "#e1e8d7";
        Values["listText"] = "#29462b";
        Record["colors"] = Values; Schemes[Name] = Record; Appearance["schemes"] = Schemes;
        QVERIFY(Settings.Put(Section::Appearance, Appearance));
        Colors->Refresh();
        auto Scheme = Colors->GetColorScheme(Name);
        QCOMPARE(Scheme.explicitColors_.size(), 2);
        QCOMPARE(Scheme.Dialog_BGColor, QColor("#e1e8d7"));
        Scheme.FileList_Cursor = QColor("#E0EBFF"); // Explicit old default.
        Scheme.explicitColors_.insert("listCursor");
        Colors->UpsertColorScheme(Scheme, true);
        QVERIFY(Settings.Commit());
        Repository Restart(QFileInfo(Settings.FileName()).absolutePath());
        const auto Saved = Restart.Read(Section::Appearance).toObject()["schemes"].toObject()[Name].toObject();
        QVERIFY(Saved["colorOverrides"].toObject()["listCursor"].toBool());
        Colors->Refresh();
        Scheme = Colors->GetColorScheme(Name);
        QCOMPARE(Scheme.FileList_Cursor, QColor("#E0EBFF"));
        QCOMPARE(Scheme.FileList_FGColor, QColor("#29462b"));
        emit Colors->sigColorSchemeChanged(Scheme);
        QCOMPARE(QApplication::palette().color(QPalette::Window), QColor("#e1e8d7"));
        for (int Index = 0; Index < 12; ++Index) {
            emit Colors->sigColorSchemeChanged(Colors->GetColorScheme(Index % 2 ? Name : QStringLiteral("기본 다크")));
            QCoreApplication::processEvents();
            QCOMPARE(qApp->style(), Style.data());
            QCOMPARE(Proxy->baseStyle(), Base.data());
        }
    }

    void hiddenViewerPreparationKeepsFocusUi()
    {
        auto* Panel = panels_.front();
        auto* View = qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex())); QVERIFY(View);
        main_->activateWindow(); QVERIFY(QTest::qWaitForWindowActive(main_));
        Panel->sig_NotifyPanelActivated(); View->EnsureKeyboardFocusOnView();
        const auto Focus = GetFocus(); QVERIFY(Focus); QPointer<QWidget> Input = QApplication::focusWidget(); QVERIFY(Input);
        SetForegroundWindow(reinterpret_cast<HWND>(main_->winId()));
        QTRY_COMPARE(GetForegroundWindow(), reinterpret_cast<HWND>(main_->winId()));
        const auto Catalog = hc::document::BuildDocumentHandlerCatalog(hc::settings::AppSettings(), QCoreApplication::applicationDirPath());
        const auto Found = Catalog.targets_.find("builtin.hyperviewer"); QVERIFY(Found != Catalog.targets_.end());
        QVERIFY2(Found->second.descriptor_.available_, "The pinned built-in viewer must be available");
        QtViewerHost Host(main_, [] { return hc::storage::MakeNativeMaterializer(QDir::tempPath().toStdString()); });
        const auto Drain = qScopeGuard([&] { Host.CloseAll(); while (!Host.IsIdle()) QTest::qWait(10); });
        int NativeLosses = 0, QtLosses = 0, ForegroundLosses = 0;
        QTimer Sample; Sample.setInterval(10);
        connect(&Sample, &QTimer::timeout, this, [&] {
            if (!ForegroundLosses && GetForegroundWindow() != reinterpret_cast<HWND>(main_->winId())) {
                DWORD Process = 0; GetWindowThreadProcessId(GetForegroundWindow(), &Process);
                qInfo() << "First foreground change during preparation, owner PID:" << Process << "HWND:" << GetForegroundWindow();
            }
            NativeLosses += GetFocus() != Focus;
            QtLosses += QApplication::focusWidget() != Input;
            ForegroundLosses += GetForegroundWindow() != reinterpret_cast<HWND>(main_->winId());
        }); Sample.start();
        Host.PreparePlugin(Found->second.plugin_);
        auto* Prepared = main_->findChild<QDialog*>("hc.document.prepared"); QVERIFY(Prepared); QVERIFY(!Prepared->isVisible());
        auto* Surface = Prepared->findChild<QtPluginViewerSurface*>(); QVERIFY(Surface);
        QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(), 15000);
        QTest::qWait(12000); Sample.stop();
        qInfo() << "Hidden preparation native/Qt/foreground focus losses:" << NativeLosses << QtLosses << ForegroundLosses;
        QCOMPARE(NativeLosses, 0); QCOMPARE(QtLosses, 0); QCOMPARE(ForegroundLosses, 0);
        QCOMPARE(GetFocus(), Focus); QCOMPARE(QApplication::focusWidget(), Input.data());
        QVERIFY(!Prepared->isVisible());
        QVERIFY(!IsWindowVisible(reinterpret_cast<HWND>(Prepared->winId())));
        QTemporaryDir Documents; QFile File(Documents.filePath("focus.txt"));
        QVERIFY(File.open(QIODevice::WriteOnly)); File.write("viewer focus fixture"); File.close();
        auto Provider = hc::storage::MakeNativeReadProvider(hc::storage::LocatorKind::Local);
        const auto Entry = Provider->Resolve({hc::storage::LocatorKind::Local, File.fileName().toStdString()}, {}).get(); QVERIFY(Entry.HasValue());
        auto Source = std::make_shared<hc::storage::ProviderContentSource>(Provider, Entry.Value());
        QPointer<QDialog> Presented = Host.OpenPlugin(Found->second.plugin_, {{Source, {}}}, "focus fixture");
        QCOMPARE(Presented.data(), Prepared); // Adopt the pinned HWND; do not recreate the native grant.
        QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(), 15000);
        QTRY_COMPARE(GetForegroundWindow(), reinterpret_cast<HWND>(Presented->winId()));
        QTRY_VERIFY(GetFocus() && IsChild(reinterpret_cast<HWND>(Presented->winId()), GetFocus()));
        Presented->reject();
        QTRY_COMPARE(GetForegroundWindow(), reinterpret_cast<HWND>(main_->winId()));
        QTRY_COMPARE(QApplication::focusWidget(), Input.data());
        QTRY_VERIFY(GetFocus());
        QTest::qWait(1000); // Includes host teardown and replenishing the hidden spare.
        QCOMPARE(QApplication::focusWidget(), Input.data()); QVERIFY(GetFocus());
    }

    void todoBugWindowReactivationFocus_data()
    {
        QTest::addColumn<int>("Target");
        QTest::addColumn<bool>("Minimize");
        QTest::addColumn<bool>("Fallback");
        for (int Target = 0; Target < 3; ++Target)
            for (const bool Minimize : {false, true})
                for (const bool Fallback : {false, true}) {
                    if (Target == 2 && Fallback) continue;
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(Target).arg(Minimize ? "minimize" : "switch")
                        .arg(Fallback ? "fallback" : "native"))) << Target << Minimize << Fallback;
                }
    }

    void todoBugWindowReactivationFocus()
    {
        QFETCH(int, Target); QFETCH(bool, Minimize); QFETCH(bool, Fallback);
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        const auto Workspace = main_->CaptureWorkspace();
        const auto Restore = qScopeGuard([&] { main_->RestoreWorkspace(Workspace); });
        for (int I = 0; I < 6; ++I) {
            QFile File(Directory.filePath(QString("entry%1.txt").arg(I)));
            QVERIFY(File.open(QIODevice::WriteOnly)); File.write("focus fixture");
        }
        auto* Panel = panels_[Target == 1 ? 1 : 0];
        Panel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {});
        auto* View = qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex())); QVERIFY(View);
        QTRY_VERIFY(!model(Panel)->IsLoading()); QTRY_COMPARE(model(Panel)->rowCount(), 7);
        auto* Command = main_->findChild<fm::ui::CommandLine*>("hc.commandLine"); QVERIFY(Command); Command->show();
        const auto Root = reinterpret_cast<HWND>(main_->winId());
        main_->showNormal(); main_->activateWindow(); SetForegroundWindow(Root);
        QTRY_COMPARE(GetForegroundWindow(), Root);
        Panel->sig_NotifyPanelActivated(); View->EnsureKeyboardFocusOnView();
        View->BaseView()->setFocusedRowIndex(2);
        if (Target == 2) { Command->lineEdit()->clear(); Command->lineEdit()->setFocus(); }
        QPointer<QWidget> Input = QApplication::focusWidget(); QVERIFY(Input);
        QVERIFY(GetFocus());
        const auto Row = View->BaseView()->focusedRowIndex();
        const auto Trace = connect(qApp, &QApplication::focusChanged, this, [](QWidget* Before, QWidget* After) {
            qInfo() << "Activation focus" << Before << "->" << After;
        });
        const auto DisconnectTrace = qScopeGuard([&] { disconnect(Trace); });
        QProcess Peer;
        Peer.start(QCoreApplication::applicationFilePath(), {"--focus-peer"});
        QTRY_VERIFY(Peer.canReadLine());
        const auto PeerWindow = reinterpret_cast<HWND>(Peer.readLine().trimmed().toULongLong()); QVERIFY(IsWindow(PeerWindow));
        const auto StopPeer = qScopeGuard([&] {
            SendMessageW(PeerWindow, WM_APP + 117, reinterpret_cast<WPARAM>(Root), 0);
            Peer.kill(); Peer.waitForFinished(); QTest::qWait(100);
        });
        for (int Cycle = 0; Cycle < 3; ++Cycle) {
            if (Minimize) {
                auto* Button = main_->findChild<QToolButton*>("fm.window.minimize"); QVERIFY(Button);
                QTest::mouseClick(Button, Qt::LeftButton);
                QTRY_VERIFY(main_->isMinimized());
            }
            QVERIFY(SetForegroundWindow(PeerWindow));
            QTRY_COMPARE(GetForegroundWindow(), PeerWindow);
            // Exercise a valid Qt fallback while the native panel is inactive.
            // A fallback must not replace the user's last input destination.
            if (Fallback) Command->lineEdit()->setFocus(Qt::ActiveWindowFocusReason);
            QTest::qWait(100);
            if (Minimize) ShowWindow(Root, SW_RESTORE);
            // The foreground peer grants activation just as the taskbar does.
            // Do not repair either Qt or native child focus from the harness.
            QVERIFY(SendMessageW(PeerWindow, WM_APP + 117, reinterpret_cast<WPARAM>(Root), 0));
            QTRY_COMPARE(GetForegroundWindow(), Root);
            QTest::qWait(150); // Let native activation and queued recovery complete without clicking a panel.
            QCOMPARE(QApplication::focusWidget(), Input.data());
            QVERIFY(GetFocus() && (GetFocus() == Root || IsChild(Root, GetFocus())));
            INPUT Keys[2]{}; Keys[0].type = Keys[1].type = INPUT_KEYBOARD;
            Keys[0].ki.wVk = Keys[1].ki.wVk = Target == 2 ? 'X' : VK_DOWN;
            Keys[1].ki.dwFlags = KEYEVENTF_KEYUP;
            QCOMPARE(SendInput(2, Keys, sizeof(INPUT)), UINT(2));
            if (Target == 2) {
                auto* Editor = qobject_cast<QLineEdit*>(Input.data()); QVERIFY(Editor);
                QTRY_COMPARE(Editor->text(), QString(Cycle + 1, 'x'));
            } else QTRY_COMPARE(View->BaseView()->focusedRowIndex(), Row + Cycle + 1);
        }
    }

    void dialogFocusReturnUi()
    {
        auto* Panel = panels_.front();
        auto* View = qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex())); QVERIFY(View);
        auto* Grid = View->findChild<Qtitan::Grid*>(); QVERIFY(Grid);
        main_->activateWindow(); QVERIFY(QTest::qWaitForWindowActive(main_));
        Panel->sig_NotifyPanelActivated(); View->EnsureKeyboardFocusOnView();
        QPointer<QWidget> Input = QApplication::focusWidget(); QVERIFY(Input);
        QDialog Dialog(main_); QVBoxLayout Layout(&Dialog); QLineEdit Editor; Layout.addWidget(&Editor);
        Dialog.show(); Dialog.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&Dialog)); Editor.setFocus();
        QTRY_COMPARE(QApplication::focusWidget(), &Editor);
        Dialog.reject(); main_->activateWindow();
        if (auto* Current = QApplication::focusWidget()) Current->clearFocus(); // A disappearing native input can leave no Qt receiver.
        QTRY_VERIFY(QApplication::focusWidget() && (QApplication::focusWidget() == Grid || Grid->isAncestorOf(QApplication::focusWidget())));
        auto* Command = main_->findChild<fm::ui::CommandLine*>("hc.commandLine"); QVERIFY(Command); Command->setFocus();
        QPointer<QWidget> CommandInput = QApplication::focusWidget(); QVERIFY(CommandInput);
        Dialog.show(); Dialog.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&Dialog)); Editor.setFocus();
        Dialog.reject(); main_->activateWindow(); QTRY_COMPARE(QApplication::focusWidget(), CommandInput.data());
        // Qt can retain its remembered widget after Windows loses the native
        // receiver during cancellation/host teardown. Normal Qt keyClick tests
        // bypass this condition, so explicitly exercise the Win32 boundary.
        main_->activateWindow(); SetForegroundWindow(reinterpret_cast<HWND>(main_->winId()));
        QTRY_COMPARE(GetForegroundWindow(), reinterpret_cast<HWND>(main_->winId()));
        qInfo() << "Native focus loss setup (foreground/root/focus):" << GetForegroundWindow() << reinterpret_cast<HWND>(main_->winId()) << GetFocus();
        SetFocus(nullptr);
        QTRY_VERIFY2(GetFocus() != nullptr, "The active main window must recover a missing native keyboard receiver");
        QTRY_COMPARE(QApplication::focusWidget(), CommandInput.data());
        QDialog Other; QLineEdit OtherEditor(&Other); Other.show(); Other.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&Other)); OtherEditor.setFocus();
        Dialog.setAttribute(Qt::WA_ShowWithoutActivating); Dialog.show(); Dialog.hide(); QTest::qWait(100);
        QCOMPARE(QApplication::activeWindow(), &Other); QCOMPARE(QApplication::focusWidget(), &OtherEditor);
        Other.close(); main_->activateWindow();
    }

    void renameCancellationFocusUi()
    {
        auto& Settings = hc::settings::AppSettings(); const auto Saved = Settings.Object();
        const auto Workspace = main_->CaptureWorkspace();
        const auto Restore = qScopeGuard([&] { Settings.Replace(Saved); main_->RestoreWorkspace(Workspace); });
        QTemporaryDir Directory; QFile File(Directory.filePath("cancel.txt"));
        QVERIFY(File.open(QIODevice::WriteOnly)); File.write("original"); File.close();
        hc::workspace::TabState Tab; Tab.id_ = "rename-focus";
        Tab.location_ = {hc::storage::LocatorKind::Local, Directory.path().toStdString()};
        hc::workspace::PaneState Pane; Pane.tabs_ = {Tab};
        auto* Panel = panels_.front(); Panel->RestoreWorkspace(Pane);
        auto* Model = model(Panel); QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading() && Model->rowCount() == 2, 10000);
        auto* View = qobject_cast<CGridView*>(Panel->GetFocusView(0)); QVERIFY(View);
        auto* Grid = View->findChild<Qtitan::Grid*>(); QVERIFY(Grid);
        auto Options = Settings.Read(hc::settings::Section::Options).toObject(); Options["renameInDialog"] = true;
        QVERIFY(Settings.Put(hc::settings::Section::Options, Options));
        for (bool LoseNative : {false, true}) {
            for (int Row = 0; Row < Model->rowCount(); ++Row)
                if (Model->GetName(Model->index(Row, 0)) == "cancel.txt")
                    View->RestoreSelection({}, hc::workspace::SelectionKey(*Model->GetFileInfo(Model->index(Row, 0))->storageEntry_));
            main_->activateWindow(); QVERIFY(QTest::qWaitForWindowActive(main_));
            Panel->sig_NotifyPanelActivated(); View->EnsureKeyboardFocusOnView();
            SetForegroundWindow(reinterpret_cast<HWND>(main_->winId()));
            QTRY_COMPARE(GetForegroundWindow(), reinterpret_cast<HWND>(main_->winId()));
            bool Cancelled = false;
            QTimer::singleShot(80, main_, [&] {
                auto* Dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                auto* Editor = Dialog ? Dialog->findChild<QLineEdit*>("hc.operation.editor") : nullptr;
                if (!Editor) return;
                Cancelled = true; QTest::keyClick(Editor, Qt::Key_Escape);
                if (LoseNative) SetFocus(nullptr);
            });
            TyStCommandMgr::GetInstance()->Bridge()->PromptFileOperation("hc.file.rename", "test");
            QVERIFY(Cancelled);
            QTRY_VERIFY(GetFocus() && QApplication::focusWidget() && (QApplication::focusWidget() == Grid || Grid->isAncestorOf(QApplication::focusWidget())));
            QCOMPARE(QFileInfo(Directory.filePath("cancel.txt")).size(), 8);
        }
    }

    void todoBugRemoteDesktopParentNavigationUi()
    {
        const auto Share = qEnvironmentVariable("HC_TEST_RDP_SHARE");
        if (Share.isEmpty()) QSKIP("An explicitly selected RDP drive is required.");
        const auto Workspace = main_->CaptureWorkspace();
        const auto Restore = qScopeGuard([&] { main_->RestoreWorkspace(Workspace); });
        const auto Root = QString("\\\\tsclient\\") + Share;
        auto* Panel = panels_.front();
        hc::workspace::TabState Tab; Tab.id_ = "rdp-parent";
        Tab.location_ = {hc::storage::LocatorKind::Smb, Root.toStdString()};
        hc::workspace::PaneState Pane; Pane.tabs_ = {Tab}; Panel->RestoreWorkspace(Pane);
        auto* Model = static_cast<CFSModel*>(model(Panel));
        QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 15000);
        QVERIFY2(!Model->LastNavigationError(), Model->LastNavigationError() ? Model->LastNavigationError()->code_.c_str() : "");
        Model->ChangeDirectory("..");
        QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 15000);
        QVERIFY2(!Model->LastNavigationError(), Model->LastNavigationError() ? Model->LastNavigationError()->code_.c_str() : "");
        QCOMPARE(QDir::fromNativeSeparators(Panel->CurrentPath()).toLower(), QString("//tsclient"));
        QVERIFY(!Panel->findChild<QMessageBox*>("hc.navigationError"));
        QVERIFY(names(Model).contains(Share, Qt::CaseInsensitive));
        Model->ChangeDirectory(Share);
        QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 15000); QVERIFY(!Model->LastNavigationError());
        QCOMPARE(QDir::fromNativeSeparators(Panel->CurrentPath()).toLower(), QDir::fromNativeSeparators(Root).toLower());
        // The legacy browsing mode must show the same redirected drives.
        nsHC::CFSSmb Legacy("\\");
        const auto Children = Legacy.GetChildItems("\\\\tsclient");
        QVERIFY(std::any_of(Children.begin(), Children.end(), [&](const auto& Item) { return Item->Name_.compare(Share, Qt::CaseInsensitive) == 0; }));
    }

    void todoBugNavigationErrorPresentationUi()
    {
        const auto Workspace = main_->CaptureWorkspace();
        const auto Restore = qScopeGuard([&] { main_->RestoreWorkspace(Workspace); });
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        auto* Panel = panels_.front();
        hc::workspace::TabState Tab; Tab.id_ = "navigation-description";
        Tab.location_ = {hc::storage::LocatorKind::Local, Directory.path().toStdString()};
        hc::workspace::PaneState Pane; Pane.tabs_ = {Tab}; Panel->RestoreWorkspace(Pane);
        auto* Model = static_cast<CFSModel*>(model(Panel)); QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 10000);
        Model->ChangeDirectory("missing-folder");
        QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading() && Model->LastNavigationError(), 10000);
        QTRY_VERIFY(Panel->findChild<QMessageBox*>("hc.navigationError"));
        QPointer<QMessageBox> Failure = Panel->findChild<QMessageBox*>("hc.navigationError");
        QVERIFY(Failure->text().contains(QStringLiteral("폴더를 찾을 수 없습니다")));
        QVERIFY(!Failure->informativeText().contains("win32:"));
        QVERIFY(Failure->detailedText().contains("win32:"));
        QVERIFY(Failure->detailedText().contains("missing-folder"));
        QVERIFY(Failure->width() >= 520);
        const auto Artifacts = qEnvironmentVariable("HC_UI_ARTIFACT_DIR");
        if (!Artifacts.isEmpty()) {
            QDir().mkpath(Artifacts);
            QVERIFY(Failure->grab().save(QDir(Artifacts).filePath("navigation-error-readable.png")));
        }
        QTest::keyClick(Failure, Qt::Key_Escape); QTRY_VERIFY(Failure.isNull());
    }

    void driveRootNavigationUi()
    {
        const auto Workspace = main_->CaptureWorkspace();
        const auto Restore = qScopeGuard([&] { main_->RestoreWorkspace(Workspace); });
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        const auto Letter = Directory.path().front().toUpper();
        const auto Root = QString(Letter) + ":/";
        const auto Id = QString("hc.navigate.gotoDrive%1").arg(Letter);
        auto* Bridge = TyStCommandMgr::GetInstance()->Bridge(); QVERIFY(Bridge);
        for (auto* Panel : panels_) {
            hc::workspace::TabState Tab; Tab.id_ = QUuid::createUuid().toString();
            Tab.location_ = {hc::storage::LocatorKind::Local, Directory.path().toStdString()};
            hc::workspace::PaneState Pane; Pane.tabs_ = {Tab}; Panel->RestoreWorkspace(Pane);
            QTRY_VERIFY_WITH_TIMEOUT(!model(Panel)->IsLoading(), 10000);
        }
        for (auto* Panel : panels_) {
            auto* View = qobject_cast<CGridView*>(Panel->GetFocusView(0)); QVERIFY(View);
            auto* Grid = View->findChild<Qtitan::Grid*>(); QVERIFY(Grid);
            Panel->sig_NotifyPanelActivated(); Grid->setFocus(); QCoreApplication::processEvents();
            const auto Other = panels_[Panel == panels_.front() ? 1 : 0];
            const auto OtherPath = Other->CurrentPath();
            QTest::keyClick(Grid, Qt::Key(Letter.unicode()), Qt::ControlModifier | Qt::ShiftModifier);
            QTRY_VERIFY_WITH_TIMEOUT(!model(Panel)->IsLoading()
                && QDir::fromNativeSeparators(Panel->CurrentPath()) == Root, 10000);
            QCOMPARE(Other->CurrentPath(), OtherPath);
            Panel->ChangeCurrentPath(Directory.path(), {});
            QTRY_VERIFY_WITH_TIMEOUT(!model(Panel)->IsLoading()
                && QDir::fromNativeSeparators(Panel->CurrentPath()) == Directory.path(), 10000);
            QVERIFY(Bridge->ExecuteCommand(Id));
            QTRY_VERIFY_WITH_TIMEOUT(!model(Panel)->IsLoading()
                && QDir::fromNativeSeparators(Panel->CurrentPath()) == Root, 10000);
            QCOMPARE(Other->CurrentPath(), OtherPath);
        }
    }

    void driveListOrderUi()
    {
        const auto Workspace = main_->CaptureWorkspace();
        const auto Restore = qScopeGuard([&] { main_->RestoreWorkspace(Workspace); });
        hc::workspace::TabState Tab; Tab.id_ = "drive-list-order";
        Tab.location_ = {hc::storage::LocatorKind::Shell, "::{20D04FE0-3AEA-1069-A2D8-08002B30309D}"};
        hc::workspace::PaneState Pane; Pane.tabs_ = {Tab};
        auto* Panel = panels_.front(); Panel->RestoreWorkspace(Pane);
        auto* Model = model(Panel); QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading() && Model->rowCount() > 0, 10000);
        auto* View = qobject_cast<CGridView*>(Panel->GetFocusView(0)); QVERIFY(View);
        QVERIFY(View->ApplyColumnProfile(hc::columns::DefaultProfile()));
        const auto Ordered = [&] {
            int Previous = -1, Drives = 0;
            for (int Row = 0; Row < View->BaseView()->getRowCount(); ++Row) {
                auto Index = View->BaseView()->getRow(Row).modelIndex(0);
                while (const auto* Proxy = qobject_cast<const QAbstractProxyModel*>(Index.model())) Index = Proxy->mapToSource(Index);
                const auto Item = Model->GetFileInfo(Index);
                if (!Item || Item->Name_ == "..") continue;
                const int Key = Item->storageEntry_ ? hc::ui::DriveLetterSortKey(*Item->storageEntry_) : 26;
                if (Key < Previous) return false;
                Previous = Key; if (Key < 26) ++Drives;
            }
            return Drives > 0;
        };
        QTRY_VERIFY(Ordered());
        auto* Name = static_cast<Qtitan::GridTableColumn*>(View->BaseView()->getColumnByModelColumn(0));
        Name->setSortOrder(Qtitan::SortDescending); QTest::qWait(160); QTRY_VERIFY(Ordered());
        Name->setSortOrder(Qtitan::SortAscending); QTest::qWait(160); QTRY_VERIFY(Ordered());
        QVERIFY(main_->grab().save(QDir(QCoreApplication::applicationDirPath()).filePath("drive-list-order.png")));
    }

    void externalFileDragUi()
    {
        const auto Workspace = main_->CaptureWorkspace();
        const auto Restore = qScopeGuard([&] { main_->RestoreWorkspace(Workspace); });
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        for (const auto& Name : {"first.txt", "한글 이름.txt"}) {
            QFile File(Directory.filePath(Name)); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("drag fixture");
        }
        hc::workspace::TabState Tab; Tab.id_ = "external-drag";
        Tab.location_ = {hc::storage::LocatorKind::Local, Directory.path().toStdString()};
        hc::workspace::PaneState Pane; Pane.tabs_ = {Tab};
        auto* Panel = panels_.front(); Panel->RestoreWorkspace(Pane);
        auto* Model = model(Panel); QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading() && Model->rowCount() == 3, 10000);
        auto* View = qobject_cast<CGridView*>(Panel->GetFocusView(0)); QVERIFY(View);
        const auto IndexFor = [&](const QString& Name) {
            for (int Row = 0; Row < Model->rowCount(); ++Row) if (Model->GetName(Model->index(Row, 0)) == Name) return Model->index(Row, 0);
            return QModelIndex{};
        };
        const auto KeyFor = [&](const QString& Name) { return hc::workspace::SelectionKey(*Model->GetFileInfo(IndexFor(Name))->storageEntry_); };
        View->RestoreSelection({}, KeyFor("first.txt"));
        QCOMPARE(View->DragPaths(IndexFor("한글 이름.txt")), QStringList{Directory.filePath("한글 이름.txt")});
        QVERIFY(View->DragPaths({}).isEmpty());
        QVERIFY(View->DragPaths(IndexFor("..")).isEmpty());
        View->RestoreSelection({KeyFor("first.txt"), KeyFor("한글 이름.txt")}, KeyFor("first.txt"));
        auto Expected = QStringList{Directory.filePath("first.txt"), Directory.filePath("한글 이름.txt")}; Expected.sort();
        auto Paths = View->DragPaths(IndexFor("first.txt")); Paths.sort(); QCOMPARE(Paths, Expected);
        QVERIFY(View->DragPaths(IndexFor("..")).isEmpty());
        auto* Grid = View->findChild<Qtitan::Grid*>(); QVERIFY(Grid);
        Panel->sig_NotifyPanelActivated(); Grid->setFocus(); QCoreApplication::processEvents();
        QPoint Press(-1, -1);
        for (const auto& Hit : Grid->hitInfoAll()) {
            if (Hit.info() == Qtitan::GridHitInfo::Cell && Hit.modelIndex().data().toString() == "first.txt") {
                Press = Hit.rect().center(); break;
            }
        }
        QVERIFY(Press.x() >= 0);
        QTest::mousePress(Grid->viewport(), Qt::LeftButton, Qt::NoModifier, Press);
        NativeDragProbe Probe(View);
        const QPoint Moved = Press + QPoint(QApplication::startDragDistance() + 5, 0);
        QMouseEvent Move(QEvent::MouseMove, Moved, Grid->viewport()->mapToGlobal(Moved), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(Grid->viewport(), &Move);
        // A synthetic press can make OLE cancel immediately on a headless desktop.
        // The drag object still records the exact native payload until deleteLater.
        if (!Probe.seen_) {
            if (const auto* Drag = View->findChild<QDrag*>()) {
                Probe.seen_ = true;
                for (const auto& Url : Drag->mimeData()->urls()) Probe.paths_.append(Url.toLocalFile());
            }
        }
        QVERIFY(Probe.seen_); Probe.paths_.sort(); QCOMPARE(Probe.paths_, Expected);
        QVERIFY(QFile::exists(Expected.front())); QVERIFY(QFile::exists(Expected.back()));
        QVERIFY(!View->IsEditing());
    }

    void shellMenuUi()
    {
        const auto Workspace = main_->CaptureWorkspace();
        const auto Restore = qScopeGuard([&] { main_->RestoreWorkspace(Workspace); });
        QTemporaryDir Directory, Other; QVERIFY(Directory.isValid() && Other.isValid());
        for (const auto& Name : {"first.txt", "second.txt", "한글 이름.txt"}) {
            QFile File(Directory.filePath(Name)); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("shell menu fixture");
        }
        hc::workspace::TabState Tab; Tab.id_ = "shell-menu";
        Tab.location_ = {hc::storage::LocatorKind::Local, Directory.path().toStdString()};
        hc::workspace::PaneState Pane; Pane.tabs_ = {Tab};
        auto* Panel = panels_.front(); Panel->RestoreWorkspace(Pane);
        auto* Model = model(Panel); QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading() && Model->rowCount() == 4, 10000);
        auto* View = qobject_cast<CGridView*>(Panel->GetFocusView(0)); QVERIFY(View);
        auto* Grid = View->findChild<Qtitan::Grid*>(); QVERIFY(Grid);
        const auto IndexFor = [&](const QString& Name) {
            for (int Row = 0; Row < Model->rowCount(); ++Row) if (Model->GetName(Model->index(Row, 0)) == Name) return Model->index(Row, 0);
            return QModelIndex{};
        };
        const auto KeyFor = [&](const QString& Name) { return hc::workspace::SelectionKey(*Model->GetFileInfo(IndexFor(Name))->storageEntry_); };
        const auto Focus = [&] {
            Panel->sig_NotifyPanelActivated(); Grid->setFocus(); QCoreApplication::processEvents();
        };
        View->RestoreSelection({}, KeyFor("한글 이름.txt")); Focus();
        QCOMPARE(View->ShellMenuPaths(), QStringList{Directory.filePath("한글 이름.txt")});
        auto* Bridge = TyStCommandMgr::GetInstance()->Bridge(); QVERIFY(Bridge);
        QVERIFY(Bridge->Action("hc.file.shellMenu")); QVERIFY(Bridge->Action("hc.file.shellMenu")->isEnabled());
        for (const auto Key : {Qt::Key_Return, Qt::Key_Enter}) {
            Focus(); ShellMenuProbe Probe;
            QTest::keyClick(Grid, Key, Key == Qt::Key_Enter ? Qt::ShiftModifier | Qt::KeypadModifier : Qt::ShiftModifier);
            QVERIFY2(Probe.opened_, "Shift+Enter must open a native Explorer menu"); QVERIFY(Probe.items_ > 0);
        }
        View->RestoreSelection({KeyFor("first.txt"), KeyFor("second.txt")}, KeyFor("한글 이름.txt")); Focus();
        auto Expected = QStringList{Directory.filePath("first.txt"), Directory.filePath("second.txt")}; Expected.sort();
        auto Selected = View->ShellMenuPaths(); Selected.sort(); QCOMPARE(Selected, Expected);
        QCOMPARE(View->ShellMenuPaths(IndexFor("한글 이름.txt"), false), QStringList{Directory.filePath("한글 이름.txt")});
        { ShellMenuProbe Probe; QVERIFY(Bridge->ExecuteCommand("hc.file.shellMenu")); QVERIFY(Probe.opened_); QVERIFY(Probe.items_ > 0); }
        { ShellMenuProbe Probe; QVERIFY(Bridge->ExecuteCommand("hc.file.context")); QVERIFY(Probe.opened_); }
        View->RestoreSelection({}, KeyFor("first.txt"));
        { ShellMenuProbe Probe; QVERIFY(View->ShowShellMenu(IndexFor("first.txt"), true)); QVERIFY(Probe.opened_); }
        { ShellMenuProbe Probe; QVERIFY(View->ShowShellMenu({}, true)); QVERIFY(Probe.opened_); }
        View->RestoreSelection({}, {});
        QCOMPARE(View->ShellMenuPaths(IndexFor(".."), false), QStringList{Directory.path()});
        const auto Owner = reinterpret_cast<void*>(main_->winId());
        QVERIFY(!OpenShellContextMenuForObjects({}, 0, 0, Owner));
        QVERIFY(!OpenShellContextMenuForObjects({"relative.txt"}, 0, 0, Owner));
        QVERIFY(!OpenShellContextMenuForObjects({Directory.filePath("missing.txt")}, 0, 0, Owner));
        QVERIFY(!OpenShellContextMenuForObjects({Directory.filePath("first.txt"), Other.path()}, 0, 0, Owner));
        QVERIFY(!OpenShellContextMenuForObjects({Directory.path()}, 0, 0, nullptr));
        QCOMPARE(QDir(Directory.path()).entryList(QDir::Files).size(), 3);
    }

    void backgroundMetadataNavigationUi()
    {
        using namespace hc::settings;
        using namespace hc::storage;
        auto* Workers = QThreadPool::globalInstance(); const int BeforeWorkers = Workers->maxThreadCount(); Workers->setMaxThreadCount(1);
        const auto RestoreWorkers = qScopeGuard([&] { Workers->setMaxThreadCount(BeforeWorkers); });
        auto& Settings = AppSettings(); const auto Before = Settings.Object(); const auto Workspace = main_->CaptureWorkspace();
        auto* Colors = TyStColorSchemeMgr::GetInstance();
        const auto Restore = qScopeGuard([&] {
            Settings.DiscardEdit(); Settings.Replace(Before); Settings.Commit();
            Colors->Refresh(); emit Colors->sigColorSchemeChanged(Colors->GetColorScheme(Colors->GetCurrentColorScheme()));
            main_->RestoreWorkspace(Workspace);
        });
        auto Options = Settings.Read(Section::Options).toObject(); Options["folderSizeMode"] = "local"; Options["folderSizeUseEverything"] = false;
        auto Rules = FileColorRules::Empty(); Rules["groups"] = QJsonArray{QJsonObject{{"id", "async-folders"}, {"name", "폴더"}, {"join", "all"},
            {"conditions", QJsonArray{QJsonObject{{"type", "folder"}, {"value", "true"}}}},
            {"light", QJsonObject{{"foreground", "#3182ce"}}}, {"dark", QJsonObject{{"foreground", "#3182ce"}}}}};
        Options["fileColorRules"] = Rules; QVERIFY(Settings.Put(Section::Options, Options));
        class SizeProvider final : public GatedProvider {
        public:
            Task<std::unique_ptr<IEntryCursor>> Enumerate(Locator At, OperationContext Context) override {
                if (At != inner_.Root()) {
                    started_ = true;
                    while (gate_.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready)
                        if (const auto Error = Context.Check(Id())) return Ready<std::unique_ptr<IEntryCursor>>(*Error);
                }
                auto Cursor = inner_.Enumerate(At, Context).get(); if (!Cursor.HasValue()) return Ready<std::unique_ptr<IEntryCursor>>(Cursor.Failure());
                auto Page = Cursor.Value()->Next(512, Context).get(); if (!Page.HasValue()) return Ready<std::unique_ptr<IEntryCursor>>(Page.Failure());
                for (auto& Entry : Page.Value().entries_) if (Entry.directory_) Entry.attributes_ |= FILE_ATTRIBUTE_DIRECTORY;
                return Ready<std::unique_ptr<IEntryCursor>>(std::unique_ptr<IEntryCursor>(new test::MemoryCursor(std::move(Page.Value().entries_), std::make_shared<test::FaultState>())));
            }
        };
        auto Provider = std::make_shared<SizeProvider>();
        for (int At = 0; At < 256; ++At) {
            const auto Id = Provider->inner_.Add(Provider->inner_.Root(), QString("folder-%1").arg(At, 3, 10, QChar('0')).toStdString(), {}, true);
            Provider->inner_.Add(Provider->inner_.Stat(Id, {}).get().Value().locator_, "content", std::vector<std::byte>(3));
        }
        bool Released = false; const auto Release = qScopeGuard([&] { if (!Released) Provider->release_.set_value(); });
        auto* Panel = panels_.front(); auto* Model = static_cast<CFSModel*>(model(Panel));
        auto* View = static_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex()));
        auto* Grid = View->findChild<Qtitan::Grid*>(); QVERIFY(Grid);
        View->SetListMode("one-line");
        Model->SetRoot(nsHC::TySpFileSystem(new GatedFs(Provider))); Model->SetCurrentPath("\\"); Model->Refresh();
        QTRY_VERIFY(!Model->IsLoading() && Model->PendingDirectorySizes() == 256 && Provider->started_.load()); Model->SetWatchEnabled(false);
        const auto First = [&] { for (int Row = 0; Row < Model->rowCount(); ++Row) if (Model->GetName(Model->index(Row, 0)) == "folder-000") return Model->index(Row, 0); return QModelIndex{}; }();
        QVERIFY(First.isValid());
        const auto FirstKey = hc::workspace::SelectionKey(*Model->GetFileInfo(First)->storageEntry_);
        View->RestoreSelection({FirstKey}, FirstKey); Panel->sig_NotifyPanelActivated(); View->EnsureKeyboardFocusOnView();
        QTest::keyClick(Grid, Qt::Key_Down); QVERIFY(View->CaptureFocus() != FirstKey);
        const auto Moved = View->CaptureFocus(); QVERIFY(!Moved.isEmpty()); QCOMPARE(Model->PendingDirectorySizes(), 256);
        // A color cache miss must return immediately, then update in a later turn.
        Model->SetColorScheme(Colors->GetColorScheme(Colors->GetCurrentColorScheme()));
        QSignalSpy Painted(Model, &QAbstractItemModel::dataChanged);
        QVERIFY(Model->data(First, Qt::ForegroundRole).value<QColor>() != QColor("#3182ce")); QCOMPARE(Painted.count(), 0);
        QTRY_COMPARE(Model->data(First, Qt::ForegroundRole).value<QColor>(), QColor("#3182ce"));
        QCOMPARE(Model->PendingDirectorySizes(), 256); QCOMPARE(View->CaptureFocus(), Moved); QVERIFY(View->CaptureSelection().contains(FirstKey));
        auto Profile = View->ColumnProfile(); int SizeColumn = -1;
        for (int Col = 0; Col < Profile.columns_.size(); ++Col) if (Profile.columns_[Col].fieldId_ == "file.size") SizeColumn = Col;
        QVERIFY(SizeColumn > 0 && Profile.columns_.size() > 2);
        const int OtherColumn = SizeColumn == 1 ? 2 : 1;
        std::swap(Profile.columns_[SizeColumn], Profile.columns_[OtherColumn]); SizeColumn = OtherColumn;
        QVERIFY(View->ApplyColumnProfile(Profile)); QCOMPARE(Model->PendingDirectorySizes(), 256);
        bool MovedDuringPublication = false; int FirstRemaining = -1;
        QSignalSpy Sizes(Model, &CFSModel::DirectorySizesChanged);
        const auto Progress = connect(Model, &CFSModel::DirectorySizesChanged, this, [&] {
            if (FirstRemaining < 0) FirstRemaining = Model->PendingDirectorySizes();
            if (!MovedDuringPublication && Model->PendingDirectorySizes() > 0) {
                const auto Focus = View->CaptureFocus(); QTest::keyClick(Grid, Qt::Key_Down);
                MovedDuringPublication = View->CaptureFocus() != Focus;
            }
        });
        Painted.clear(); Provider->release_.set_value(); Released = true;
        QTRY_COMPARE_WITH_TIMEOUT(Model->PendingDirectorySizes(), 0, 10000); disconnect(Progress);
        QVERIFY(FirstRemaining >= 224); QVERIFY(MovedDuringPublication); QVERIFY(Sizes.count() >= 8);
        QCOMPARE(Model->GetTotalSize(), 256u * 3u); QVERIFY(View->CaptureSelection().contains(FirstKey));
        for (const auto& Signal : Painted) if (Signal[2].value<QList<int>>().contains(Qt::DisplayRole))
            QCOMPARE(Signal[0].value<QModelIndex>().column(), SizeColumn);
        // Leaving while a provider is still calculating cancels without joining it.
        auto Pending = std::make_shared<SizeProvider>();
        const auto Id = Pending->inner_.Add(Pending->inner_.Root(), "waiting", {}, true);
        Pending->inner_.Add(Pending->inner_.Stat(Id, {}).get().Value().locator_, "late", std::vector<std::byte>(7));
        const auto ReleasePending = qScopeGuard([&] { Pending->release_.set_value(); });
        Model->SetRoot(nsHC::TySpFileSystem(new GatedFs(Pending))); Model->SetCurrentPath("\\"); Model->Refresh();
        QTRY_VERIFY(!Model->IsLoading() && Pending->started_.load());
        QTemporaryDir Destination; QVERIFY(Destination.isValid());
        { QFile File(Destination.filePath("arrived.txt")); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("new"); }
        QElapsedTimer Leave; Leave.start(); Panel->ChangeCurrentPath(Destination.path(), {});
        QVERIFY2(Leave.elapsed() < 250, "Leaving joined a background metadata worker");
        QTRY_VERIFY(!Model->IsLoading() && names(Model).contains("arrived.txt"));
        QTest::qWait(80); QVERIFY(!names(Model).contains("waiting")); QCOMPARE(Model->GetTotalSize(), 3u);
    }

    void folderSizesUi()
    {
        using namespace hc::settings;
        auto& Settings = AppSettings(); const auto Original = Settings.Object(); const auto Workspace = main_->CaptureWorkspace();
        auto* Commands = TyStCommandMgr::GetInstance();
        const auto Restore = qScopeGuard([&] { Settings.DiscardEdit(); Settings.Replace(Original); Commands->Refresh(); main_->RestoreWorkspace(Workspace); });
        auto Options = Settings.Read(Section::Options).toObject();
        Options["folderSizeMode"] = "manual"; Options["folderSizeUseEverything"] = false; Options["folderSizeOnSpace"] = false;
        QVERIFY(Settings.Put(Section::Options, Options));
        auto General = Settings.Read(Section::General).toObject(); General["sizeInFooter"] = "bytes"; QVERIFY(Settings.Put(Section::General, General));
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        QDir Root(Directory.path()); QVERIFY(Root.mkpath("alpha/inner")); QVERIFY(Root.mkpath("beta")); QVERIFY(Root.mkpath("empty"));
        for (const auto& Pair : {qMakePair(QString("alpha/one"), QByteArray("123")), qMakePair(QString("alpha/inner/two"), QByteArray("12345")), qMakePair(QString("beta/three"), QByteArray("1234567890123"))}) {
            QFile File(Directory.filePath(Pair.first)); QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write(Pair.second), Pair.second.size());
        }
        hc::workspace::TabState Tab; Tab.id_ = "folder-sizes"; Tab.location_ = {hc::storage::LocatorKind::Local, Directory.path().toStdString()};
        hc::workspace::PaneState Pane; Pane.tabs_ = {Tab}; auto* Panel = panels_.front(); Panel->RestoreWorkspace(Pane);
        auto OtherPane = Pane; OtherPane.tabs_[0].id_ = "folder-sizes-other"; panels_.back()->RestoreWorkspace(OtherPane);
        auto* Model = qobject_cast<CFSModel*>(model(Panel)); QVERIFY(Model);
        QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading() && Model->rowCount() == 4, 10000);
        Model->SetWatchEnabled(false);
        auto* View = qobject_cast<CGridView*>(Panel->GetFocusView(0)); QVERIFY(View); auto* Grid = View->findChild<Qtitan::Grid*>(); QVERIFY(Grid);
        const auto IndexFor = [&](const QString& Name) { for (int Row = 0; Row < Model->rowCount(); ++Row) if (Model->GetName(Model->index(Row, 0)) == Name) return Model->index(Row, 0); return QModelIndex{}; };
        const auto Item = [&](const QString& Name) { return Model->GetFileInfo(IndexFor(Name)); };
        const auto Focus = [&] {
            View->RestoreSelection({}, hc::workspace::SelectionKey(*Item("alpha")->storageEntry_));
            main_->activateWindow(); SetForegroundWindow(reinterpret_cast<HWND>(main_->winId()));
            Panel->sig_NotifyPanelActivated(); Grid->setFocus(); QCoreApplication::processEvents();
        };
        QVERIFY(!Item("alpha")->directorySize_); QCOMPARE(Model->PendingDirectorySizes(), 0);
        Focus(); QTest::keyClick(Grid, Qt::Key_Space);
        QTRY_VERIFY(View->CaptureSelection().contains(hc::workspace::SelectionKey(*Item("alpha")->storageEntry_)));
        QCOMPARE(Model->PendingDirectorySizes(), 0); QVERIFY(!Item("alpha")->directorySize_);
        Options["folderSizeOnSpace"] = true; QVERIFY(Settings.Put(Section::Options, Options));
        Focus(); QTest::keyClick(Grid, Qt::Key_Space);
        QTRY_VERIFY_WITH_TIMEOUT(Item("alpha")->directorySize_.has_value(), 10000); QCOMPARE(*Item("alpha")->directorySize_, 8u);
        QVERIFY(View->CaptureSelection().contains(hc::workspace::SelectionKey(*Item("alpha")->storageEntry_)));
        QTRY_VERIFY2(Panel->findChild<QLabel*>("lblStatus")->text().contains("8 bytes / 8 bytes"), qPrintable(Panel->findChild<QLabel*>("lblStatus")->text()));
        QTest::keyClick(Grid, Qt::Key_Return, Qt::AltModifier | Qt::ShiftModifier);
        QTRY_VERIFY_WITH_TIMEOUT(Item("empty")->directorySize_.has_value() && Item("beta")->directorySize_.has_value(), 10000);
        QCOMPARE(*Item("empty")->directorySize_, 0u); QCOMPARE(*Item("beta")->directorySize_, 13u); QCOMPARE(Model->GetTotalSize(), 21u);
        { QFile File(Directory.filePath("beta/three")); QVERIFY(File.open(QIODevice::WriteOnly | QIODevice::Truncate)); QCOMPARE(File.write("12345678901234"), 14); }
        QTest::keyClick(Grid, Qt::Key_Return, Qt::AltModifier | Qt::ShiftModifier);
        QTRY_VERIFY(Item("beta")->directorySize_ && *Item("beta")->directorySize_ == 14u); QCOMPARE(Model->GetTotalSize(), 22u);
        { QFile File(Directory.filePath("beta/three")); QVERIFY(File.open(QIODevice::WriteOnly | QIODevice::Truncate)); QCOMPARE(File.write("1234567890123"), 13); }
        QTest::keyClick(Grid, Qt::Key_Enter, Qt::AltModifier | Qt::ShiftModifier | Qt::KeypadModifier);
        QTRY_VERIFY(Item("beta")->directorySize_ && *Item("beta")->directorySize_ == 13u); QCOMPARE(Model->GetTotalSize(), 21u);
        Options["folderSizeMode"] = "everything"; QVERIFY(Settings.Put(Section::Options, Options));
        Model->Refresh(); QTRY_VERIFY(!Model->IsLoading());
        QVERIFY(!Item("alpha")->directorySize_); QCOMPARE(Model->PendingDirectorySizes(), 0);
        Options["folderSizeMode"] = "local"; QVERIFY(Settings.Put(Section::Options, Options));
        Model->ApplyDirectorySizeSettings(); QCOMPARE(Model->PendingDirectorySizes(), 3);
        Model->RequestDirectorySizes({IndexFor("alpha")}); QCOMPARE(Model->PendingDirectorySizes(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(Item("alpha")->directorySize_.has_value() && Model->PendingDirectorySizes() == 0, 10000);
        QCOMPARE(*Item("alpha")->directorySize_, 8u);
        // A tracker refresh (file change or periodic rescan) keeps measured sizes on screen while re-measuring.
        QTRY_VERIFY_WITH_TIMEOUT(Item("beta")->directorySize_ && Item("empty")->directorySize_ && Model->PendingDirectorySizes() == 0, 10000);
        const auto Calculating = [&](const QString& Name) {
            const auto Row = IndexFor(Name).row();
            for (int Column = 0; Column < Model->columnCount(); ++Column)
                if (Model->index(Row, Column).data().toString() == QString::fromUtf8("계산 중…")) return true;
            return false;
        };
        const auto Total = Model->GetTotalSize();
        Model->SetWatchEnabled(true);
        { QFile File(Directory.filePath("added.txt")); QVERIFY(File.open(QIODevice::WriteOnly)); }
        QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading() && Model->rowCount() == 5, 10000);
        QVERIFY(Item("alpha")->directorySize_ && *Item("alpha")->directorySize_ == 8u); QVERIFY(!Calculating("alpha")); QVERIFY(!Calculating("beta"));
        QCOMPARE(Model->GetTotalSize(), Total);
        QTRY_VERIFY_WITH_TIMEOUT(Model->PendingDirectorySizes() == 0, 10000);
        QCOMPARE(*Item("beta")->directorySize_, 13u); QCOMPARE(Model->GetTotalSize(), Total);
        QVERIFY(QFile::remove(Directory.filePath("added.txt"))); QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading() && Model->rowCount() == 4, 10000);
        Model->SetWatchEnabled(false);
        // Mixed search results must reject network folders before clearing a known size or queuing I/O.
        for (const auto& Remote : {hc::storage::Locator{hc::storage::LocatorKind::Smb, "\\\\unreachable.invalid\\share\\alpha"},
                 hc::storage::Locator{hc::storage::LocatorKind::Local, "\\\\?\\UNC\\unreachable.invalid\\share\\alpha"}}) {
            const auto Folder = Item("alpha"); const auto Previous = Folder->storageEntry_->locator_;
            const auto RestoreLocation = qScopeGuard([&] { Folder->storageEntry_->locator_ = Previous; });
            Folder->storageEntry_->locator_ = Remote;
            Model->RequestDirectorySizes({IndexFor("alpha")});
            QCOMPARE(Model->PendingDirectorySizes(), 0); QVERIFY(Folder->directorySize_); QCOMPARE(*Folder->directorySize_, 8u);
        }
        { QMainOpts Dialog(main_); Dialog.LoadSettings(); Dialog.show(); QTest::qWait(150);
            auto* Categories = Dialog.findChild<QListWidget*>("lstOptCate"); QVERIFY(Categories);
            for (int Row = 0; Row < Categories->count(); ++Row) if (Categories->item(Row)->text() == "파일 작업") Categories->setCurrentRow(Row);
            auto* Tabs = Dialog.findChild<QTabWidget*>("hc.settings.fileOperationsTabs"); QVERIFY(Tabs); Tabs->setCurrentIndex(0);
            auto* Page = Dialog.findChild<QScrollArea*>("hc.settings.fileOperationsCommon"); QVERIFY(Page);
            auto* Mode = Page->findChild<QComboBox*>("hc.settings.folderSizeMode"); QVERIFY(Mode); QCOMPARE(Mode->count(), 4);
            QVERIFY(Page->findChild<QCheckBox*>("hc.settings.folderSizeOnSpace"));
            QVERIFY(!Dialog.findChild<QGroupBox*>("groupBox"));
            Mode->setCurrentIndex(2); Page->findChild<QCheckBox*>("hc.settings.folderSizeBeforeOperations")->setChecked(false);
            QVERIFY(Dialog.SaveSettings()); QCOMPARE(Settings.Read(Section::Options).toObject()["folderSizeMode"].toString(), QString("all"));
            QCOMPARE(Settings.Read(Section::Options).toObject()["folderSizeBeforeOperations"].toBool(), false);
            QVERIFY(SaveR9Dialog(Dialog, "folder-size-common")); Dialog.reject(); }
        { QMainOpts Dialog(main_); Dialog.LoadSettings();
            QCOMPARE(Dialog.findChild<QComboBox*>("hc.settings.folderSizeMode")->currentIndex(), 2);
            QVERIFY(!Dialog.findChild<QCheckBox*>("hc.settings.folderSizeBeforeOperations")->isChecked()); Dialog.reject(); }
    }

    void inlineRenameClicksUi_data()
    {
        QTest::addColumn<bool>("TwoLine"); QTest::addColumn<bool>("IncludeExtension");
        for (const bool TwoLine : {false, true}) for (const bool IncludeExtension : {false, true})
            QTest::newRow(qPrintable(QString("%1-extension-%2").arg(TwoLine ? "two-line" : "one-line").arg(IncludeExtension))) << TwoLine << IncludeExtension;
    }
    void inlineRenameClicksUi()
    {
        using namespace hc::settings;
        QFETCH(bool, TwoLine); QFETCH(bool, IncludeExtension);
        auto& Settings = AppSettings(); const auto Original = Settings.Object(); const auto Workspace = main_->CaptureWorkspace();
        auto* Commands = TyStCommandMgr::GetInstance();
        const auto Restore = qScopeGuard([&] {
            for (auto* View : main_->findChildren<CGridView*>()) {
                View->CancelRename(); if (View->BaseView()->activeEditor()) View->BaseView()->hideEditor();
            }
            Settings.DiscardEdit(); Settings.Replace(Original); Commands->Refresh(); main_->RestoreWorkspace(Workspace);
        });
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        for (const auto* Name : {"A.tar.gz", "B.txt"}) { QFile File(Directory.filePath(Name)); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("click rename"); }
        QVERIFY(QDir().mkpath(Directory.filePath("subdir")));
        auto* Left = main_->findChild<CmpPanel*>("cmpLeftPanel"); auto* Right = main_->findChild<CmpPanel*>("cmpRightPanel"); QVERIFY(Left && Right);
        hc::workspace::TabState Tab; Tab.id_ = "rename-click-left"; Tab.location_ = {hc::storage::LocatorKind::Local, Directory.path().toStdString()};
        hc::workspace::PaneState Pane; Pane.tabs_ = {Tab}; Left->RestoreWorkspace(Pane); Pane.tabs_[0].id_ = "rename-click-right"; Right->RestoreWorkspace(Pane);
        QTRY_VERIFY(!model(Left)->IsLoading() && !model(Right)->IsLoading());
        QTRY_COMPARE(model(Left)->rowCount(), 4); QTRY_COMPARE(model(Right)->rowCount(), 4);
        auto* View = static_cast<CGridView*>(Left->GetFocusView(0)); auto* Other = static_cast<CGridView*>(Right->GetFocusView(0));
        auto* Grid = View->findChild<Qtitan::Grid*>(); auto* OtherGrid = Other->findChild<Qtitan::Grid*>(); QVERIFY(Grid && OtherGrid);
        QVERIFY(View->SetListMode(TwoLine ? "two-line" : "one-line")); QVERIFY(Other->SetListMode("one-line"));
        auto Options = Settings.Read(Section::Options).toObject();
        // The dialog option describes the rename key; a mouse rename stays in its cell.
        Options["renameInDialog"] = IncludeExtension; Options["renameSelectExtension"] = IncludeExtension;
        Options["renameOnSlowClick"] = true;
        QVERIFY(Settings.Put(Section::Options, Options)); Commands->Refresh();
        main_->showNormal(); main_->activateWindow(); SetForegroundWindow(reinterpret_cast<HWND>(main_->winId()));
        QVERIFY(QTest::qWaitForWindowActive(main_)); QCoreApplication::processEvents();
        const auto CellPoint = [](Qtitan::Grid* Target, const QString& Name) {
            for (const auto& Hit : Target->hitInfoAll()) {
                if (Hit.info() != Qtitan::GridHitInfo::Cell || Hit.modelIndex().column() != 0) continue;
                auto Index = Hit.modelIndex();
                while (const auto* Proxy = qobject_cast<const QAbstractProxyModel*>(Index.model())) Index = Proxy->mapToSource(Index);
                const auto* Model = qobject_cast<const CFSModel*>(Index.model());
                if (Model && Model->GetName(Index) == Name) return Hit.rect().center();
            }
            return QPoint(-1, -1);
        };
        const auto A = CellPoint(Grid, "A.tar.gz"), LocalB = CellPoint(Grid, "B.txt"), B = CellPoint(OtherGrid, "B.txt");
        QVERIFY(A.x() >= 0 && LocalB.x() >= 0 && B.x() >= 0);
        const int Settle = QApplication::doubleClickInterval() + 100;
        // A row focused by the keyboard has not received a rename click yet.
        for (const auto& Hit : Grid->hitInfoAll())
            if (Hit.info() == Qtitan::GridHitInfo::Cell && Hit.rect().contains(A)) View->BaseView()->setFocusedRowIndex(Hit.row().rowIndex());
        Left->sig_NotifyPanelActivated(); Grid->setFocus(); QCoreApplication::processEvents();
        QTest::mouseClick(Grid->viewport(), Qt::LeftButton, Qt::NoModifier, A);
        QTest::qWait(Settle); QVERIFY2(!View->IsEditing(), "The first click after keyboard selection must not start editing");
        QTest::mouseClick(Grid->viewport(), Qt::LeftButton, Qt::NoModifier, LocalB);
        QTest::mouseClick(Grid->viewport(), Qt::LeftButton, Qt::NoModifier, A);
        QTest::qWait(Settle); QVERIFY2(!View->IsEditing(), "A to B to A within one panel must only change the focused file");
        QTest::mouseClick(Grid->viewport(), Qt::LeftButton, Qt::NoModifier, A);
        QTest::mouseClick(OtherGrid->viewport(), Qt::LeftButton, Qt::NoModifier, B);
        QTest::mouseClick(Grid->viewport(), Qt::LeftButton, Qt::NoModifier, A);
        QTest::qWait(Settle); QVERIFY(View->IsPanelActive()); QVERIFY(!Other->IsPanelActive());
        QVERIFY2(!View->IsEditing(), "Returning to the previously focused file must only activate its panel");
        QVERIFY(!Other->IsEditing());
        QTest::qWait(3 * QApplication::doubleClickInterval() + 100);
        QTest::mouseClick(Grid->viewport(), Qt::LeftButton, Qt::NoModifier, A);
        QTest::qWait(Settle); QVERIFY2(!View->IsEditing(), "A stale selected name needs a fresh first click");
        QTest::mouseClick(Grid->viewport(), Qt::LeftButton, Qt::NoModifier, A);
        QLineEdit* Input = nullptr;
        QTRY_VERIFY((Input = View->findChild<QLineEdit*>("hc.file.renameEditor")) && Input->isVisible());
        QCOMPARE(Input->text(), QString("A.tar.gz")); QCOMPARE(Input->selectedText(), IncludeExtension ? QString("A.tar.gz") : QString("A.tar"));
        QVERIFY(!View->BaseView()->activeEditor());
        QTest::keyClick(Input, Qt::Key_F2);
        QCOMPARE(Input->selectedText(), IncludeExtension ? QString("A.tar") : QString("A.tar.gz"));
        QTest::keyClick(Input, Qt::Key_Escape); QVERIFY(!View->IsEditing());
        QTest::mouseClick(Grid->viewport(), Qt::LeftButton, Qt::NoModifier, A);
        QTest::mouseClick(Grid->viewport(), Qt::LeftButton, Qt::ControlModifier, A);
        QTest::qWait(Settle); QVERIFY(!View->IsEditing());
        QTest::mouseClick(Grid->viewport(), Qt::LeftButton, Qt::NoModifier, A);
        QTest::keyClick(Grid, Qt::Key_Shift);
        QTest::mouseClick(Grid->viewport(), Qt::LeftButton, Qt::NoModifier, A);
        QTest::qWait(Settle); QVERIFY2(!View->IsEditing(), "Keyboard input must reset the name click sequence");
        // A second click can request a delayed rename, then activation of the
        // other panel must cancel that request before its editor appears.
        QTest::keyClick(Grid, Qt::Key_Escape);
        QTest::mouseClick(Grid->viewport(), Qt::LeftButton, Qt::NoModifier, A);
        QTest::qWait(Settle);
        QTest::mouseClick(Grid->viewport(), Qt::LeftButton, Qt::NoModifier, A);
        QTest::mouseClick(OtherGrid->viewport(), Qt::LeftButton, Qt::NoModifier, B);
        QTest::qWait(Settle); QVERIFY(!View->IsEditing()); QVERIFY(!Other->IsEditing());
        Options["renameOnSlowClick"] = false; QVERIFY(Settings.Put(Section::Options, Options)); Commands->Refresh();
        QTest::mouseClick(Grid->viewport(), Qt::LeftButton, Qt::NoModifier, A);
        QTest::qWait(Settle);
        QTest::mouseClick(Grid->viewport(), Qt::LeftButton, Qt::NoModifier, A);
        QTest::qWait(Settle); QVERIFY2(!View->IsEditing(), "The disabled click rename option must prevent editing");
        Options["renameOnSlowClick"] = true; QVERIFY(Settings.Put(Section::Options, Options)); Commands->Refresh();
        QVERIFY(QFileInfo::exists(Directory.filePath("A.tar.gz")));
        const auto Folder = CellPoint(Grid, "subdir"); QVERIFY(Folder.x() >= 0);
        QTest::mouseClick(Grid->viewport(), Qt::LeftButton, Qt::NoModifier, Folder);
        QTest::qWait(Settle);
        QTest::mouseClick(Grid->viewport(), Qt::LeftButton, Qt::NoModifier, Folder);
        QTest::mouseDClick(Grid->viewport(), Qt::LeftButton, Qt::NoModifier, Folder);
        QTest::mouseRelease(Grid->viewport(), Qt::LeftButton, Qt::NoModifier, Folder);
        QTRY_COMPARE(QDir::cleanPath(QDir::fromNativeSeparators(model(Left)->GetCurrentPathWithRoot())), QDir::cleanPath(Directory.filePath("subdir")));
        QTest::qWait(Settle); QVERIFY(!View->IsEditing());
    }
    void inlineRenameGeometryUi() { hc::tests::InlineRenameGeometryUi(main_, panels_.front()); }
    void inlineRenameUi()
    {
        using namespace hc::settings;
        auto& Settings = AppSettings(); const auto Original = Settings.Object();
        const auto Workspace = main_->CaptureWorkspace();
        auto* Commands = TyStCommandMgr::GetInstance();
        const auto Restore = qScopeGuard([&] {
            for (auto* View : main_->findChildren<CGridView*>()) View->CancelRename();
            Settings.DiscardEdit(); Settings.Replace(Original); Commands->Refresh(); main_->RestoreWorkspace(Workspace);
        });
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        for (const auto& Name : {"0.txt", "a.tar.gz", "b.txt", "c.txt"}) {
            QFile File(Directory.filePath(Name)); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("rename fixture");
        }
        hc::workspace::TabState Tab; Tab.id_ = "inline-rename";
        Tab.location_ = {hc::storage::LocatorKind::Local, Directory.path().toStdString()};
        hc::workspace::PaneState Pane; Pane.tabs_ = {Tab};
        auto* Panel = panels_.front(); Panel->RestoreWorkspace(Pane);
        auto* Model = model(Panel); QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading() && Model->rowCount() == 5, 10000);
        auto* View = qobject_cast<CGridView*>(Panel->GetFocusView(0)); QVERIFY(View);
        auto* Grid = View->findChild<Qtitan::Grid*>(); QVERIFY(Grid);
        QVERIFY(View->SetListMode("one-line"));
        const auto IndexFor = [&](const QString& Name) {
            for (int Row = 0; Row < Model->rowCount(); ++Row) if (Model->GetName(Model->index(Row, 0)) == Name) return Model->index(Row, 0);
            return QModelIndex{};
        };
        const auto Focus = [&](const QString& Name) {
            const auto Item = Model->GetFileInfo(IndexFor(Name));
            if (Item && Item->storageEntry_) View->RestoreSelection({}, hc::workspace::SelectionKey(*Item->storageEntry_));
            Panel->sig_NotifyPanelActivated(); Grid->setFocus(); QCoreApplication::processEvents();
        };
        const auto Input = [&]() -> QLineEdit* {
            for (auto* Edit : View->findChildren<QLineEdit*>("hc.file.renameEditor")) if (Edit->isVisible()) return Edit;
            return nullptr;
        };
        auto Options = Settings.Read(Section::Options).toObject();
        Options["renameInDialog"] = false; Options["renameSelectExtension"] = false; Options["renameOnSlowClick"] = true;
        QVERIFY(Settings.Put(Section::Options, Options)); Commands->Refresh();
        main_->activateWindow(); SetForegroundWindow(reinterpret_cast<HWND>(main_->winId()));
        Focus("a.tar.gz"); QTest::keyClick(Grid, Qt::Key_F2);
        QTRY_VERIFY(Input()); QCOMPARE(Input()->text(), QString("a.tar.gz")); QCOMPARE(Input()->selectedText(), QString("a.tar"));
        QTest::keyClick(Input(), Qt::Key_F2); QCOMPARE(Input()->selectedText(), QString("a.tar.gz"));
        QTest::keyClick(Input(), Qt::Key_F2); QCOMPARE(Input()->selectedText(), QString("a.tar"));
        QVERIFY(SaveR9Dialog(*main_, "inline-rename-name"));
        const auto RepeatKey = [&](Qt::Key Key) {
            QKeyEvent Event(QEvent::KeyPress, Key, Qt::NoModifier, QString{}, true, 1);
            QCoreApplication::sendEvent(Input(), &Event);
        };
        // A held arrow continues across replacement editors without key releases.
        // Keep this traversal independent of asynchronous filesystem resorting.
        QTest::keyPress(Input(), Qt::Key_Down);
        QTRY_VERIFY(Input() && Input()->text() == "b.txt");
        for (const auto& Name : {"c.txt"}) {
            qInfo() << "Repeat down from" << Input()->text() << "to" << Name;
            RepeatKey(Qt::Key_Down);
            QTRY_VERIFY2_WITH_TIMEOUT(Input() && Input()->text() == Name,
                qPrintable(Input() ? Input()->text() : QString("no rename editor")), 15000);
            QVERIFY(Input()->hasFocus());
        }
        for (const auto& Name : {"b.txt", "a.tar.gz"}) {
            qInfo() << "Repeat up from" << Input()->text() << "to" << Name;
            RepeatKey(Qt::Key_Up);
            QTRY_VERIFY_WITH_TIMEOUT(Input() && Input()->text() == Name, 15000);
            QVERIFY(Input()->hasFocus());
        }
        QTest::keyRelease(Input(), Qt::Key_Down); QTest::keyRelease(Input(), Qt::Key_Up);
        RepeatKey(Qt::Key_F2); QCOMPARE(Input()->selectedText(), QString("a.tar"));
        Input()->setText("z.tar.gz"); QTest::keyClick(Input(), Qt::Key_Down);
        QTRY_VERIFY2_WITH_TIMEOUT(QFileInfo::exists(Directory.filePath("z.tar.gz")), qPrintable(main_->statusBar()->currentMessage()), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(Input() && Input()->text() == "b.txt", 15000);
        QCOMPARE(Input()->selectedText(), QString("b"));
        Input()->setText("b-renamed.txt"); QTest::keyClick(Input(), Qt::Key_Up);
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(Directory.filePath("b-renamed.txt")), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(Input() && Input()->text() == "0.txt", 15000);
        QTest::keyClick(Input(), Qt::Key_Escape); QVERIFY(!View->IsEditing());
        QVERIFY(!QFileInfo::exists(Directory.filePath("a.tar.gz")));
        QVERIFY(QFileInfo::exists(Directory.filePath("c.txt")));

        Options["renameSelectExtension"] = true; QVERIFY(Settings.Put(Section::Options, Options));
        Focus("c.txt"); QTest::keyClick(Grid, Qt::Key_F2); QTRY_VERIFY(Input());
        QCOMPARE(Input()->selectedText(), QString("c.txt"));
        QTest::keyClick(Input(), Qt::Key_F2); QCOMPARE(Input()->selectedText(), QString("c"));
        Input()->setText("cancelled.txt"); QTest::keyClick(Input(), Qt::Key_Escape);
        QVERIFY(!View->IsEditing()); QVERIFY(!QFileInfo::exists(Directory.filePath("cancelled.txt")));

        Commands->SetShortcutWithCMD(QKeyCombination(Qt::AltModifier, Qt::Key_R), "hc.file.rename");
        Focus("c.txt"); QTest::keyClick(Grid, Qt::Key_R, Qt::AltModifier); QTRY_VERIFY(Input());
        QTest::keyClick(Input(), Qt::Key_R, Qt::AltModifier); QCOMPARE(Input()->selectedText(), QString("c"));
        Input()->setText("bad/name.txt"); QTest::keyClick(Input(), Qt::Key_Return);
        QTRY_VERIFY(Input() && !Input()->isReadOnly()); QCOMPARE(Input()->text(), QString("bad/name.txt"));
        QTest::keyClick(Input(), Qt::Key_Escape); QVERIFY(QFileInfo::exists(Directory.filePath("c.txt")));

        Options["renameInDialog"] = true; QVERIFY(Settings.Put(Section::Options, Options));
        Focus("c.txt"); QString Selected;
        QTimer::singleShot(80, main_, [&] {
            if (auto* Dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
                if (auto* Edit = Dialog->findChild<QLineEdit*>("hc.operation.editor")) Selected = Edit->selectedText();
                Dialog->reject();
            }
        });
        Commands->Bridge()->PromptFileOperation("hc.file.rename", "test"); QCOMPARE(Selected, QString("c.txt"));
        { QMainOpts Dialog(main_); Dialog.show(); Dialog.LoadSettings();
            auto* Tabs = Dialog.findChild<QTabWidget*>("hc.settings.fileOperationsTabs");
            auto* Page = Dialog.findChild<QWidget*>("hc.settings.fileOperationsRename"); QVERIFY(Tabs && Page);
            auto* Categories = Dialog.findChild<QListWidget*>("lstOptCate");
            for (int Row = 0; Row < Categories->count(); ++Row) if (Categories->item(Row)->text() == "파일 작업") Categories->setCurrentRow(Row);
            Tabs->setCurrentWidget(Page);
            auto* Separate = Dialog.findChild<QCheckBox*>("hc.settings.renameInDialog");
            auto* Extension = Dialog.findChild<QCheckBox*>("hc.settings.renameSelectExtension");
            auto* Click = Dialog.findChild<QCheckBox*>("hc.settings.renameOnSlowClick");
            QVERIFY(Separate && Extension && Click); QVERIFY(Separate->isChecked()); QVERIFY(Extension->isChecked()); QVERIFY(Click->isChecked());
            Separate->setChecked(false); Extension->setChecked(false); Click->setChecked(false); QVERIFY(Dialog.SaveSettings());
            QCOMPARE(Settings.Read(Section::Options).toObject()["renameInDialog"].toBool(), false);
            QCOMPARE(Settings.Read(Section::Options).toObject()["renameSelectExtension"].toBool(), false);
            QCOMPARE(Settings.Read(Section::Options).toObject()["renameOnSlowClick"].toBool(), false);
            Dialog.LoadSettings(); QVERIFY(!Click->isChecked());
            QVERIFY(SaveR9Dialog(Dialog, "inline-rename-settings")); Dialog.reject(); }
        Focus("0.txt"); QTest::keyClick(Grid, Qt::Key_F2); QTRY_VERIFY(Input());
        QTest::keyClick(Input(), Qt::Key_Up); QTRY_VERIFY(!View->IsEditing());
        QVERIFY(QFileInfo::exists(Directory.filePath("0.txt")));
        QVERIFY(View->SetListMode("two-line"));
        Focus("c.txt"); QTest::keyClick(Grid, Qt::Key_F2); QTRY_VERIFY(Input());
        QCOMPARE(Input()->selectedText(), QString("c"));
        Input()->setText("c-final.txt"); QTest::keyClick(Input(), Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(Directory.filePath("c-final.txt")), 15000);
        QTRY_VERIFY(!View->IsEditing()); QVERIFY(!QFileInfo::exists(Directory.filePath("c.txt")));
    }

    void watercolorGridStateUi()
    {
        // Anchor captures to the source tree so test working directories stay clean.
        const QDir Artifacts(QDir(QString::fromUtf8(QT_TESTCASE_SOURCEDIR))
            .absoluteFilePath("../_build/ui-validation/watercolor"));
        QVERIFY(QDir().mkpath(Artifacts.absolutePath()));
        auto* Colors = TyStColorSchemeMgr::GetInstance();
        const auto Original = Colors->GetColorScheme(Colors->GetCurrentColorScheme());
        const auto Workspace = main_->CaptureWorkspace();
        const auto Restore = qScopeGuard([&] {
            emit Colors->sigColorSchemeChanged(Original);
            main_->RestoreWorkspace(Workspace);
        });
        QTemporaryDir Directory;
        QVERIFY(Directory.isValid());
        for (int Index = 0; Index < 150; ++Index) {
            QFile File(Directory.filePath(QString("theme-%1.txt").arg(Index, 3, 10, QChar('0'))));
            QVERIFY(File.open(QIODevice::WriteOnly)); File.write("theme");
        }
        hc::workspace::TabState Tab; Tab.id_ = "theme-state";
        Tab.location_ = {hc::storage::LocatorKind::Local, Directory.path().toStdString()};
        hc::workspace::PaneState Pane; Pane.tabs_ = {Tab};
        auto* Panel = panels_.front(); Panel->RestoreWorkspace(Pane);
        auto* Model = model(Panel);
        QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading() && Model->rowCount() == 151, 10000);
        auto* View = qobject_cast<CGridView*>(Panel->GetFocusView(0)); QVERIFY(View);
        auto* Grid = View->findChild<Qtitan::Grid*>(); QVERIFY(Grid);
        auto* Base = View->BaseView();
        Panel->sig_NotifyPanelActivated();
        Base->setFocusedColumnIndex(0); Base->setFocusedRowIndex(80);
        Base->selectRow(80); Base->selectRow(81);
        QCoreApplication::processEvents();
        auto* Column = static_cast<Qtitan::GridTableColumn*>(Base->getColumnByModelColumn(0));
        Column->setWidth(280);
        const auto Focus = View->CaptureFocus();
        const auto Selection = View->CaptureSelection();
        QVERIFY(Base->showEditor(Qtitan::GridEditor::NotActivate));
        auto* Editor = Base->activeEditor(); QVERIFY(Editor);
        auto* Input = qobject_cast<QLineEdit*>(Editor->getCellWidget()); QVERIFY(Input);
        Input->selectAll();
        QTest::keyClicks(Input, "pending-name.txt");
        QVERIFY(Editor->isContextModified());
        Input->setSelection(7, 4);
        const auto Cursor = Input->cursorPosition();
        QTest::qWait(100); // Let editor activation and deferred column sizing settle.
        const auto Scroll = Base->verticalScrollBar()->value();
        QVERIFY(Scroll > 0);
        const auto Layout = View->CaptureWorkspaceView();
        for (int Index = 0; Index < 8; ++Index) {
            emit Colors->sigColorSchemeChanged(Colors->GetColorScheme(Index % 2
                ? QStringLiteral("기본 라이트") : QStringLiteral("기본 다크")));
            QCoreApplication::processEvents();
            QCOMPARE(Base->activeEditor(), Editor);
            QCOMPARE(Input->text(), QStringLiteral("pending-name.txt"));
            QVERIFY(Editor->isContextModified());
            QCOMPARE(Input->cursorPosition(), Cursor);
            QCOMPARE(Input->selectedText(), QStringLiteral("-nam"));
            QCOMPARE(View->CaptureFocus(), Focus);
            QCOMPARE(View->CaptureSelection(), Selection);
            QCOMPARE(Base->verticalScrollBar()->value(), Scroll);
            QCOMPARE(View->CaptureWorkspaceView(), Layout);
            QVERIFY(Grid->grab().save(Artifacts.filePath(Index % 2 ? "watercolor-light.png" : "watercolor-dark.png")));
        }
        Base->hideEditor(); // Cancel the pending rename.
        QVERIFY(QFileInfo::exists(Directory.filePath("theme-079.txt")));
        QVERIFY(!QFileInfo::exists(Directory.filePath("pending-name.txt")));
    }

    void firstRunAppearanceUi()
    {
        using namespace hc::settings;
        auto& Settings = AppSettings(); const auto Original = Settings.Object();
        auto* Colors = TyStColorSchemeMgr::GetInstance();
        const auto Workspace = main_->CaptureWorkspace();
        const auto Restore = qScopeGuard([&] {
            Settings.DiscardEdit(); Settings.Replace(Original); Colors->Refresh();
            emit Colors->sigColorSchemeChanged(Colors->GetColorScheme(Colors->GetCurrentColorScheme()));
            main_->RestoreWorkspace(Workspace);
        });
        QVERIFY(Settings.Exists());
        QCOMPARE(Colors->GetNames().size(), 2);
        for (auto* Panel : panels_) {
            auto* View = qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex())); QVERIFY(View);
            const auto Model = static_cast<CFSModel*>(model(Panel));
            QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 10000);
            QVERIFY(!Model->LastNavigationError());
            QCOMPARE(QDir::fromNativeSeparators(QString::fromStdString(Model->WorkspaceLocation().root_)), QDir::homePath());
            const auto Columns = View->BaseView();
            QCOMPARE(Columns->options().backgroundColor(), Colors->GetColorScheme(Colors->GetCurrentColorScheme()).FileList_BGColor);
            QTRY_VERIFY(Columns->getColumnByModelColumn(0)->width() >= 200);
            QVERIFY(Columns->getColumnByModelColumn(1)->width() >= 88);
            QVERIFY(Columns->getColumnByModelColumn(2)->width() >= 190);
            QVERIFY(Columns->getColumnByModelColumn(3)->width() >= 56);
        }
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        { QFile File(Directory.filePath("first-run-example.txt")); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("first run"); }
        hc::workspace::TabState Tab; Tab.id_ = "first-run-theme"; Tab.location_ = {hc::storage::LocatorKind::Local, Directory.path().toStdString()};
        hc::workspace::PaneState Pane; Pane.tabs_ = {Tab}; panels_.front()->RestoreWorkspace(Pane);
        auto* Model = model(panels_.front());
        QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading() && names(Model).contains("first-run-example.txt"), 10000);
        for (const bool Dark : {true, false}) {
            const auto Name = Dark ? QStringLiteral("기본 다크") : QStringLiteral("기본 라이트");
            QMainOpts Dialog(main_); Dialog.show(); QCoreApplication::processEvents();
            Dialog.findChild<QComboBox*>("hc.settings.theme")->setCurrentIndex(Dark ? 1 : 0);
            Dialog.findChild<QPushButton*>("btnApply")->click();
            QVERIFY(Settings.Error().isEmpty());
            QCOMPARE(qApp->style()->property("hc.theme").toString(), Name);
            QCOMPARE(QApplication::palette().color(QPalette::Window), Colors->GetColorScheme(Name).Dialog_BGColor);
            const auto Scheme = Colors->GetColorScheme(Name);
            QTRY_COMPARE(Model->index(0, 0).data(Qt::ForegroundRole).value<QColor>(), Scheme.FileList_FGColor);
            QCOMPARE(Model->index(0, 0).data(Qt::BackgroundRole).value<QColor>(), Scheme.FileList_BGColor);
            auto* View = qobject_cast<CGridView*>(panels_.front()->GetFocusView(panels_.front()->CurrentTabIndex()));
            auto* Grid = View->findChild<Qtitan::Grid*>(); QVERIFY(Grid);
            QTRY_COMPARE(View->BaseView()->options().backgroundColor(), Scheme.FileList_BGColor);
            const auto EmptyBackground = [this, Grid] {
                const auto Image = main_->screen()->grabWindow(main_->winId()).toImage();
                const auto Point = Grid->viewport()->mapTo(main_, QPoint(Grid->viewport()->width() / 2, Grid->viewport()->height() - 12));
                return Image.pixelColor(qRound(Point.x() * Image.devicePixelRatio()), qRound(Point.y() * Image.devicePixelRatio()));
            };
            QTRY_COMPARE(EmptyBackground(), Scheme.FileList_BGColor);
            QCOMPARE(Model->index(0, 0).data(Qt::FontRole).value<QFont>().pointSize(), 12);
            Repository Restart(QFileInfo(Settings.FileName()).absolutePath());
            QCOMPARE(Restart.Read(Section::Appearance).toObject()["selected"].toString(), Name);
            const auto Applied = QApplication::palette();
            Dialog.findChild<QComboBox*>("hc.settings.theme")->setCurrentIndex(Dark ? 0 : 1);
            Dialog.reject(); QCOMPARE(QApplication::palette(), Applied);
            main_->showMaximized(); QTRY_VERIFY(main_->isMaximized());
            QTRY_COMPARE(EmptyBackground(), Scheme.FileList_BGColor);
            main_->showNormal(); QTRY_VERIFY(!main_->isMaximized());
            QTRY_COMPARE(EmptyBackground(), Scheme.FileList_BGColor);
        }
    }

    void appearanceInteractionUi()
    {
        using namespace hc::settings;
        auto& Settings = AppSettings(); const auto Original = Settings.Object();
        auto* Colors = TyStColorSchemeMgr::GetInstance(); const auto Workspace = main_->CaptureWorkspace();
        const auto Restore = qScopeGuard([&] {
            Settings.DiscardEdit(); Settings.Replace(Original); Settings.Commit(); Colors->Refresh();
            emit Colors->sigColorSchemeChanged(Colors->GetColorScheme(Colors->GetCurrentColorScheme()));
            main_->RestoreWorkspace(Workspace);
        });
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        for (const auto& Name : {"alpha.txt", "beta.txt", "gamma.txt"}) {
            QFile File(Directory.filePath(Name)); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("appearance");
        }
        hc::workspace::TabState Tab; Tab.id_ = "appearance"; Tab.location_ = {hc::storage::LocatorKind::Local, Directory.path().toStdString()};
        hc::workspace::PaneState Pane; Pane.tabs_ = {Tab}; auto* Panel = panels_.front(); Panel->RestoreWorkspace(Pane);
        auto* Model = model(Panel); auto* View = qobject_cast<CGridView*>(Panel->GetFocusView(0)); QVERIFY(View);
        auto* Grid = View->findChild<Qtitan::Grid*>(); QVERIFY(Grid);
        QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading() && names(Model).contains("gamma.txt"), 10000);
        auto* Base = View->BaseView();
        Base->selectRow(2); Base->selectRow(3); Base->setFocusedRowIndex(2);
        const auto Selected = View->CaptureSelection(); QVERIFY(!Selected.isEmpty());
        for (const bool Dark : {false, true, false}) {
            const auto Name = Dark ? QStringLiteral("기본 다크") : QStringLiteral("기본 라이트");
            for (int Mask = 0; Mask < 4; ++Mask) {
                QMainOpts Dialog(main_); Dialog.show(); QCoreApplication::processEvents();
                Dialog.findChild<QComboBox*>("hc.settings.theme")->setCurrentIndex(Dark ? 1 : 0);
                Dialog.findChild<QCheckBox*>("hc.settings.inverseCursor")->setChecked(Mask & 1);
                Dialog.findChild<QCheckBox*>("hc.settings.inverseSelection")->setChecked(Mask & 2);
                Dialog.findChild<QSpinBox*>("hc.settings.dialogFontSize")->setValue(Dark ? 18 : 11);
                QVERIFY(Dialog.SaveSettings());
                auto* Pages = Dialog.findChild<QStackedWidget*>("stkOpts"); QVERIFY(Pages);
                Pages->setCurrentWidget(Dialog.findChild<QWidget*>("pgDisplay"));
                QCoreApplication::processEvents();
                for (const auto& Id : {"cbxHeaderSizeDisplay", "cbxFooterSizeDisplay"}) {
                    auto* Combo = Dialog.findChild<QComboBox*>(Id); QVERIFY(Combo->isVisible());
                    QVERIFY(Combo->height() >= Combo->fontMetrics().height() + 4);
                    QVERIFY(Combo->width() >= Combo->fontMetrics().horizontalAdvance(Combo->currentText()) + 32);
                }
                Dialog.findChild<QListWidget*>("lstOptCate")->setCurrentRow(0);
                QCoreApplication::processEvents();
                const auto Scheme = Colors->GetColorScheme(Name);
                QCOMPARE(Scheme.IsInverseCursor, bool(Mask & 1)); QCOMPARE(Scheme.IsInverseSelect, bool(Mask & 2));
                QCOMPARE(Base->fileManagerOptions().invertedCursor(), bool(Mask & 1));
                QCOMPARE(Base->fileManagerOptions().invertedSelection(), bool(Mask & 2));
                QCOMPARE(View->CaptureSelection(), Selected);
                Repository Restart(QFileInfo(Settings.FileName()).absolutePath());
                const auto Saved = Restart.Read(Section::Appearance).toObject()["schemes"].toObject()[Name].toObject();
                QCOMPARE(Saved["inverseCursor"].toBool(), bool(Mask & 1));
                QCOMPARE(Saved["inverseSelection"].toBool(), bool(Mask & 2));
                for (auto* Combo : Dialog.findChildren<QComboBox*>()) {
                    QVERIFY2(Combo->sizeHint().height() >= Combo->fontMetrics().height() + 4, qPrintable(Combo->objectName()));
                    if (Combo->isVisible()) QVERIFY2(Combo->height() >= Combo->fontMetrics().height() + 4, qPrintable(Combo->objectName()));
                }
                if (Mask == 3) {
                    QTest::qWait(200); // Let checkbox and button color animations settle before evidence capture.
                    Dialog.grab().save(Dark ? "appearance-settings-dark.png" : "appearance-settings-light.png");
                }
                Dialog.accept(); QCoreApplication::processEvents();
                Panel->sig_NotifyPanelActivated();
                Base->setFocusedRowIndex(2);
                auto* Column = static_cast<Qtitan::GridTableColumn*>(Base->getColumnByModelColumn(0));
                Qtitan::GridEditorStyleOption Paint;
                qtn_init_editor_option(Base, &Paint, Column->dataBinding(), Base->getRow(2), 0,
                    false, true, true, true, false, QRect(0, 0, 200, 30), Qt::AlignLeft);
                auto Text = Scheme.FileList_FGColor; auto Background = Scheme.FileList_Selected;
                if (Mask & 2) qSwap(Text, Background);
                if (Mask & 1) qSwap(Text, Background);
                QCOMPARE(Paint.palette.color(QPalette::Text), Text); QCOMPARE(Paint.backgroundBrush.color(), Background);
                const auto Focus = View->CaptureFocus();
                auto* Other = panels_.front() == Panel ? panels_.back() : panels_.front();
                Other->sig_NotifyPanelActivated();
                QVERIFY(!Base->options().showFocusDecoration());
                qtn_init_editor_option(Base, &Paint, Column->dataBinding(), Base->getRow(2), 0,
                    false, true, true, true, false, QRect(0, 0, 200, 30), Qt::AlignLeft);
                if (Mask & 1) qSwap(Text, Background); // Inactive panes retain marks, but never apply cursor inversion.
                QCOMPARE(Paint.palette.color(QPalette::Text), Text); QCOMPARE(Paint.backgroundBrush.color(), Background);
                QCOMPARE(View->CaptureFocus(), Focus); QCOMPARE(View->CaptureSelection(), Selected);
                Panel->sig_NotifyPanelActivated();
                QVERIFY(Base->options().showFocusDecoration());
                QCOMPARE(View->CaptureFocus(), Focus); QCOMPARE(View->CaptureSelection(), Selected);
                Grid->viewport()->repaint();
                QRect Header;
                for (const auto& Hit : Grid->hitInfoAll())
                    if (Hit.info() == Qtitan::GridHitInfo::Column && Hit.columnIndex() == 1) Header = Hit.rect();
                QVERIFY(!Header.isEmpty());
                const auto Pixels = Grid->viewport()->grab().toImage(); const auto Dpr = Pixels.devicePixelRatio();
                for (const int Row : {2, 3}) {
                    QRect Cell;
                    for (const auto& Hit : Grid->hitInfoAll())
                        if (Hit.info() == Qtitan::GridHitInfo::Cell && Hit.columnIndex() == 0 && Hit.row().rowIndex() == Row) Cell = Hit.rect();
                    QVERIFY(!Cell.isEmpty());
                    qtn_init_editor_option(Base, &Paint, Column->dataBinding(), Base->getRow(Row), 0,
                        false, true, true, Row == 2, false, Cell, Qt::AlignLeft);
                    // R3 paints records independently of Qtitan's legacy editor palette.
                    // Keep the legacy settings assertions above, and check real active
                    // selection/cursor fills against the product record colors here.
                    using fm::style::Token;
                    const auto Fill = (Mask & 1) && Row == 2 ? Token::InvCur : (Mask & 2) ? Token::InvSel : Token::Sel;
                    QCOMPARE(Pixels.pixelColor(qRound((Cell.right() - 5) * Dpr), qRound(Cell.center().y() * Dpr)), View->RecordColors()[Fill]);
                }
                QImage ExpectedHeader(Header.size(), QImage::Format_ARGB32_Premultiplied);
                ExpectedHeader.fill(Qt::transparent);
                QStyleOptionHeader HeaderOption;
                HeaderOption.initFrom(Grid);
                HeaderOption.rect = ExpectedHeader.rect();
                HeaderOption.state = QStyle::State_Enabled | QStyle::State_Active;
                QPainter HeaderPainter(&ExpectedHeader);
                qApp->style()->drawControl(QStyle::CE_HeaderSection, &HeaderOption, &HeaderPainter, Grid);
                HeaderPainter.end();
                QCOMPARE(Pixels.pixelColor(qRound((Header.left() + 5) * Dpr), qRound((Header.top() + 5) * Dpr)),
                    ExpectedHeader.pixelColor(5, 5));
                if (Mask == 3) main_->grab().save(Dark ? "appearance-dark.png" : "appearance-light.png");
            }
        }
        const auto Before = Settings.Object();
        { QMainOpts Dialog(main_); Dialog.show(); QCoreApplication::processEvents();
          auto* Toggle = Dialog.findChild<QCheckBox*>("hc.settings.inverseCursor"); Toggle->setChecked(!Toggle->isChecked()); Dialog.reject(); }
        QCOMPARE(Settings.Object(), Before);
    }

    void legacySettingsDraftUi()
    {
        auto Fixture = hc::defaults::Settings();
        auto Appearance = Fixture["appearance"].toObject(); auto Schemes = Appearance["schemes"].toObject();
        auto Dark = Schemes[QStringLiteral("기본 다크")].toObject(); QJsonObject Flags;
        for (const auto& Key : Dark["colors"].toObject().keys()) Flags[Key] = true;
        Dark["colorOverrides"] = Flags; Dark["future"] = "retained"; Schemes[QStringLiteral("기본 다크")] = Dark;
        Appearance["schemes"] = Schemes; Appearance["selected"] = QStringLiteral("기본 다크"); Fixture["appearance"] = Appearance;
        auto General = Fixture["general"].toObject(); General["themeMode"] = "dark"; Fixture["general"] = General;
        auto Plugins = Fixture["plugins"].toObject(); Plugins["wlx"] = QJsonArray{QJsonObject{{"path", "old.wlx64"}, {"detect", "EXT=\"TXT\""}, {"future", true}}}; Fixture["plugins"] = Plugins;
        Fixture["future"] = QJsonObject{{"retain", true}};
        hc::ui::LegacySettingsPage Page; Page.Load(Fixture);
        QVERIFY(Page.Prepare()); auto Unchanged = Fixture; QVERIFY(Page.Apply(Unchanged)); QCOMPARE(Unchanged, Fixture);
        auto* Cursor = Page.findChild<QCheckBox*>("hc.legacy.override.listCursor"); QVERIFY(Cursor); QVERIFY(Cursor->isChecked()); Cursor->setChecked(false);
        Page.findChild<QComboBox*>("hc.legacy.defaultView")->setEditText("external.viewer");
        Page.findChild<QComboBox*>("hc.legacy.selectionPolicy")->setEditText("comparison-only");
        Page.findChild<QTableWidget*>("hc.legacy.wlxDetect")->item(0, 1)->setText("EXT=\"MD\"");
        auto* Standard = Page.findChild<QPlainTextEdit*>("hc.legacy.standard"); Standard->setPlainText("{\"dark\":{\"seeds\":{\"invcur\":\"#445566\"}}}");
        auto Current = Fixture; auto NewGeneral = Current["general"].toObject(); NewGeneral["monoFontPx"] = 23; Current["general"] = NewGeneral;
        auto NewPlugins = Current["plugins"].toObject(); auto Wlx = NewPlugins["wlx"].toArray(); auto Item = Wlx[0].toObject(); Item["hostManifest"] = "new-manifest.json"; Wlx[0] = Item;
        Wlx.append(QJsonObject{{"path", "added.wlx64"}, {"detect", ""}}); NewPlugins["wlx"] = Wlx; Current["plugins"] = NewPlugins;
        QVERIFY(Page.Prepare()); QVERIFY(Page.Apply(Current));
        QCOMPARE(Current["general"].toObject()["monoFontPx"].toInt(), 23);
        const auto SavedDark = Current["appearance"].toObject()["schemes"].toObject()[QStringLiteral("기본 다크")].toObject();
        QVERIFY(!SavedDark["colorOverrides"].toObject()["listCursor"].toBool()); QVERIFY(SavedDark["colorOverrides"].toObject()["listText"].toBool()); QCOMPARE(SavedDark["future"].toString(), QString("retained"));
        QCOMPARE(Current["plugins"].toObject()["wlx"].toArray().size(), 2);
        QCOMPARE(Current["plugins"].toObject()["wlx"].toArray()[0].toObject()["hostManifest"].toString(), QString("new-manifest.json"));
        QCOMPARE(Current["plugins"].toObject()["wlx"].toArray()[0].toObject()["detect"].toString(), QString("EXT=\"MD\""));
        QCOMPARE(Current["editors"].toObject()["documentHandlers"].toObject()["defaultViewHandlerId"].toString(), QString("external.viewer"));
        QCOMPARE(Current["editors"].toObject()["documentHandlers"].toObject()["selectionPolicy"].toString(), QString("comparison-only"));
        QCOMPARE(Current["future"], Fixture["future"]);
        Page.Load(Current); QVERIFY(Page.Prepare()); auto Again = Current; QVERIFY(Page.Apply(Again)); QCOMPARE(Again, Current);
        auto* Text = Page.findChild<QLineEdit*>("hc.legacy.color.listCursor"); Cursor->setChecked(true); Text->setText("invalid");
        QVERIFY(!Page.Prepare()); QVERIFY(!Page.findChild<QLabel*>("hc.legacy.error")->text().isEmpty());
        Text->setText("#445566"); QVERIFY(Page.Prepare()); QVERIFY(Page.Apply(Again));
        auto Removed = Current; auto RemovedPlugins = Removed["plugins"].toObject(); RemovedPlugins["wlx"] = QJsonArray{}; Removed["plugins"] = RemovedPlugins;
        Page.Load(Current); Page.findChild<QTableWidget*>("hc.legacy.wlxDetect")->item(0, 1)->setText("EXT=\"LOG\""); QVERIFY(Page.Prepare());
        const auto BeforeFailure = Removed; QVERIFY(!Page.Apply(Removed)); QCOMPARE(Removed, BeforeFailure);
    }

    void legacySettingsApplyCancelUi()
    {
        using namespace hc::settings;
        auto& Settings = AppSettings(); const auto Original = Settings.Object();
        auto* Colors = TyStColorSchemeMgr::GetInstance(); const auto OriginalScheme = Colors->GetColorScheme(Colors->GetCurrentColorScheme());
        const auto Restore = qScopeGuard([&] { Settings.DiscardEdit(); Settings.Replace(Original); Settings.Commit(); Colors->Refresh(); emit Colors->sigColorSchemeChanged(OriginalScheme); TyStCommandMgr::GetInstance()->Refresh(); });
        auto Fixture = hc::defaults::Settings(); auto Appearance = Fixture["appearance"].toObject(); auto Schemes = Appearance["schemes"].toObject();
        auto Light = Schemes[QStringLiteral("기본 라이트")].toObject(); QJsonObject Flags;
        for (const auto& Key : Light["colors"].toObject().keys()) Flags[Key] = true;
        Light["colorOverrides"] = Flags; Light["inverseCursor"] = true; Schemes[QStringLiteral("기본 라이트")] = Light; Appearance["schemes"] = Schemes; Fixture["appearance"] = Appearance;
        Fixture["fileSets"] = QJsonArray{QJsonObject{{"name", "legacy-test"}, {"flags", 2}, {"filters", QJsonArray{}}, {"extensions", QJsonArray{"txt"}}, {"attributes", 0}, {"size", 0}, {"future", "retained"}}};
        QVERIFY(Settings.Replace(Fixture)); QVERIFY(Settings.Commit()); Colors->Refresh(); TyStCommandMgr::GetInstance()->Refresh();
        const auto ReadDisk = [&] { QFile File(Settings.FileName()); if (!File.open(QIODevice::ReadOnly)) return QByteArray{}; return File.readAll(); };
        const auto Before = ReadDisk();
        { QMainOpts Dialog(main_); Dialog.show(); QTest::qWait(60); Dialog.LoadSettings();
          auto* Page = Dialog.findChild<QWidget*>("hc.settings.legacy"); QVERIFY(Page);
          Dialog.findChild<QCheckBox*>("hc.legacy.override.listCursor")->setChecked(false); Dialog.reject(); }
        QCOMPARE(ReadDisk(), Before);
        QMainOpts Dialog(main_); Dialog.show(); QTest::qWait(60); Dialog.LoadSettings();
        auto* Categories = Dialog.findChild<QListWidget*>("lstOptCate"); const auto Matches = Categories->findItems(QStringLiteral("레거시"), Qt::MatchExactly); QCOMPARE(Matches.size(), 1); Categories->setCurrentItem(Matches.front());
        Dialog.findChild<QCheckBox*>("hc.legacy.override.listCursor")->setChecked(false);
        Dialog.findChild<QCheckBox*>("hc.legacy.builtInShortcuts")->setChecked(false);
        Dialog.findChild<QSpinBox*>("hc.settings.monoSize")->setValue(17);
        QVERIFY(Dialog.SaveSettings());
        QVERIFY(!Settings.Read(Section::Appearance).toObject()["schemes"].toObject()[QStringLiteral("기본 라이트")].toObject()["colorOverrides"].toObject()["listCursor"].toBool());
        QVERIFY(!Settings.Read(Section::General).toObject()["useBuiltInShortcuts"].toBool(true));
        QCOMPARE(Settings.Read(Section::General).toObject()["monoFontPx"].toInt(), 17);
        QCOMPARE(Settings.Read(Section::FileSets).toArray()[0].toObject()["future"].toString(), QString("retained"));
        QVERIFY(TyStCommandMgr::GetInstance()->Retrieve().isEmpty());
        QVERIFY(!Colors->GetColorScheme(Colors->GetCurrentColorScheme()).explicitColors_.contains("listCursor"));
        auto* PanelView = static_cast<CGridView*>(panels_.front()->GetFocusView(panels_.front()->CurrentTabIndex()));
        QVERIFY(PanelView->ListAppearance().invertCursor);
        QCOMPARE(PanelView->RecordColors()[fm::style::Token::InvCur], fm::style::themeColorsFor(PanelView)[fm::style::Token::InvCur]);
        QVERIFY(Dialog.SaveSettings()); const auto Applied = ReadDisk();
        Dialog.findChild<QCheckBox*>("hc.legacy.override.listCursor")->setChecked(true);
        Dialog.findChild<QLineEdit*>("hc.legacy.color.listCursor")->setText("invalid");
        QVERIFY(!Dialog.SaveSettings()); QCOMPARE(ReadDisk(), Applied);
        Dialog.reject(); QCOMPARE(ReadDisk(), Applied);
        Repository Restart(QFileInfo(Settings.FileName()).absolutePath()); QVERIFY(!Restart.Read(Section::General).toObject()["useBuiltInShortcuts"].toBool(true));
    }

    void legacySettingsLayoutUi()
    {
        auto& Settings = hc::settings::AppSettings(); const auto Original = Settings.Object();
        auto* Colors = TyStColorSchemeMgr::GetInstance(); const auto OriginalScheme = Colors->GetColorScheme(Colors->GetCurrentColorScheme());
        const auto Restore = qScopeGuard([&] { Settings.DiscardEdit(); Settings.Replace(Original); Settings.Commit(); Colors->Refresh(); emit Colors->sigColorSchemeChanged(OriginalScheme); });
        for (int Mode = 0; Mode < 2; ++Mode) {
            QMainOpts Dialog(main_); Dialog.show(); QTest::qWait(60); Dialog.LoadSettings();
            Dialog.findChild<QComboBox*>("hc.settings.theme")->setCurrentIndex(Mode); QVERIFY(Dialog.SaveSettings());
            auto* Categories = Dialog.findChild<QListWidget*>("lstOptCate"); Categories->setCurrentItem(Categories->findItems(QStringLiteral("레거시"), Qt::MatchExactly).front());
            auto* Tabs = Dialog.findChild<QTabWidget*>("hc.legacy.tabs"); QCOMPARE(Tabs->count(), 4);
            Dialog.resize(1000, 720);
            for (int Tab = 0; Tab < Tabs->count(); ++Tab) {
                Tabs->setCurrentIndex(Tab); QTest::qWait(30);
                QVERIFY(Dialog.findChild<QPushButton*>("btnApply")->isVisible());
                QVERIFY(Dialog.height() <= Dialog.screen()->availableGeometry().height());
                const auto Path = QDir(QCoreApplication::applicationDirPath()).filePath(QString("legacy-%1-%2.png").arg(Mode ? "dark" : "light").arg(Tab)); QVERIFY(Dialog.grab().save(Path));
            }
            Dialog.reject();
        }
    }

    void jsonSettingsUi()
    {
        using namespace hc::settings;
        auto& Settings = AppSettings();
        const auto Original = Settings.Object();
        const auto Restore = qScopeGuard([&] { Settings.DiscardEdit(); Settings.Replace(Original); TyStCommandMgr::GetInstance()->Refresh(); });
        const auto DiskBytes = [&] { QFile File(Settings.FileName()); if (!File.open(QIODevice::ReadOnly)) return QByteArray{}; return File.readAll(); };
        QVERIFY(Settings.Put(Section::Appearance, QJsonObject{{"selected","z-selected"},
            {"schemes",QJsonObject{{"a-first",QJsonObject{}},{"z-selected",QJsonObject{}}}}}));
        QVERIFY(Settings.Commit()); const auto Before = DiskBytes(); const int Header = Settings.SizeStyle(true);
        {
            QMainOpts Dialog(main_); Dialog.show(); QCoreApplication::processEvents();
            QVERIFY(!Dialog.findChild<QWidget*>("pgDisplay_ColorScheme"));
            QCOMPARE(Settings.Read(Section::Appearance).toObject()["selected"].toString(), QString("z-selected"));
            Dialog.findChild<QComboBox*>("cbxHeaderSizeDisplay")->setCurrentIndex((Header + 1) % 12);
            Dialog.reject();
        }
        QCOMPARE(Settings.SizeStyle(true), Header); QCOMPARE(DiskBytes(), Before);
        {
            QMainOpts Dialog(main_); Dialog.show(); QCoreApplication::processEvents();
            Dialog.findChild<QComboBox*>("cbxHeaderSizeDisplay")->setCurrentIndex((Header + 1) % 12);
            auto* Apply = Dialog.findChild<QPushButton*>("btnApply"); QVERIFY(Apply);
            Apply->click(); QVERIFY(Settings.Error().isEmpty());
            Dialog.findChild<QComboBox*>("cbxHeaderSizeDisplay")->setCurrentIndex((Header + 2) % 12);
            QVERIFY(Apply->isEnabled()); Apply->click(); QVERIFY(Settings.Error().isEmpty());
            Dialog.findChild<QComboBox*>("cbxHeaderSizeDisplay")->setCurrentIndex((Header + 3) % 12);
            Dialog.reject();
        }
        Repository Restart(QFileInfo(Settings.FileName()).absolutePath()); QCOMPARE(Restart.SizeStyle(true), (Header + 2) % 12);
        auto* Commands = TyStCommandMgr::GetInstance();
        const auto Command = GlobalBuiltInCMDs.front().Name;
        const QKeyCombination Key(Qt::ControlModifier | Qt::AltModifier, Qt::Key_F12);
        Commands->SetShortcutWithCMD(Key, QString(Command)); QVERIFY(Settings.Reload()); Commands->Refresh();
        QCOMPARE(Commands->Retrieve().value(Key), Command);
        Commands->DelShortcut(Key); QVERIFY(Settings.Reload()); Commands->Refresh(); QVERIFY(!Commands->Retrieve().contains(Key));
        // A disk edit after the dialog was opened must keep the dialog and the newer file intact.
        QMainOpts Dialog(main_); Dialog.show(); QCoreApplication::processEvents();
        auto External = Settings.Object(); External["externalEdit"] = true;
        External["shortcuts"] = QJsonObject{{QKeySequence(Key).toString(QKeySequence::PortableText), QString(Command)}};
        { QFile File(Settings.FileName()); QVERIFY(File.open(QIODevice::WriteOnly)); File.write(QJsonDocument(External).toJson()); }
        const auto Newer = DiskBytes();
        QTimer Dismiss; connect(&Dismiss, &QTimer::timeout, &Dialog, [&] {
            for (auto* Box : Dialog.findChildren<QMessageBox*>()) Box->reject();
        }); Dismiss.start(10);
        QVERIFY(!Dialog.SaveSettings()); Dismiss.stop(); QVERIFY(Dialog.isVisible()); QCOMPARE(DiskBytes(),Newer);
        QTimer Reload; connect(&Reload, &QTimer::timeout, &Dialog, [&] {
            for (auto* Box : Dialog.findChildren<QMessageBox*>())
                for (auto* Button : Box->buttons()) if (Box->buttonRole(Button) == QMessageBox::ResetRole) Button->click();
        }); Reload.start(10);
        QVERIFY(!Dialog.SaveSettings()); Reload.stop(); QCOMPARE(Commands->Retrieve().value(Key), Command);
        QCOMPARE(DiskBytes(), Newer);
        Dialog.reject();
    }

    void qtitanCacheAndSortUi() { hc::tests::GridCacheAndSortRegression(); }
    void qtitanFileManagerUi() { hc::tests::GridFileManagerRegression(); }

    void listLayoutRestartUi()
    {
        if (!qEnvironmentVariableIsSet("HC_LIST_LAYOUT_RESTART")) QSKIP("Child-process restart check");
        auto* Left = main_->findChild<CmpPanel*>("cmpLeftPanel");
        QCOMPARE(Left->CaptureWorkspace().tabs_.size(), 2);
        for (int Tab = 0; Tab < 2; ++Tab) {
            auto* View = static_cast<CGridView*>(Left->GetFocusView(Tab));
            QCOMPARE(View->BaseModel()->columnCount(), 10);
            QCOMPARE(View->ColumnWidths()->LayoutMode(), QString("name-last"));
            const auto Profile = View->ColumnProfile();
            int Sizes = 0;
            for (const auto& Column : Profile.columns_) {
                Sizes += Column.fieldId_ == "file.size";
                if (Column.id_ == Profile.primaryNameId_) QCOMPARE(Column.fieldId_, QString("file.stem"));
            }
            QCOMPARE(Sizes, 2);
        }
    }

    void columnWdxUi()
    {
        using namespace hc::columns; using namespace hc::plugins;
        auto& Settings = hc::settings::AppSettings(); const auto Saved = Settings.Read(hc::settings::Section::Plugins);
        const auto Workspace = main_->CaptureWorkspace();
        const auto Restore = qScopeGuard([&] { Settings.Put(hc::settings::Section::Plugins, Saved); PluginFields::Instance().Refresh(); main_->RestoreWorkspace(Workspace); });
        QTemporaryDir Directory; const auto Binary = Directory.filePath("data.dll");
        QVERIFY(QFile::copy(QCoreApplication::applicationDirPath() + "/hc_tc_data_fixture.dll", Binary));
        QFile File(Binary); QVERIFY(File.open(QIODevice::ReadOnly));
        PluginManifest Manifest; Manifest.id_ = "columns.wdx"; Manifest.version_ = "1"; Manifest.binary_ = "data.dll";
        Manifest.sha256_ = QCryptographicHash::hash(File.readAll(), QCryptographicHash::Sha256).toHex().toStdString(); File.close();
        Manifest.format_ = PluginFormat::Wdx; Manifest.extensions_ = {Extension::Metadata}; Manifest.permissions_ = {Permission::ReadContent};
        const auto ManifestPath = Directory.filePath("wdx.json"); QFile Json(ManifestPath); QVERIFY(Json.open(QIODevice::WriteOnly)); Json.write(EncodeManifest(Manifest)); Json.close();
        Settings.SetPlugins("wdx", QJsonArray{QJsonObject{{"path", Binary}, {"hostEnabled", true}, {"hostManifest", ManifestPath}, {"hostReviewed", hc::settings::JsonObject(EncodeManifest(Manifest))}}});
        QSignalSpy Catalog(&PluginFields::Instance(), &PluginFields::Changed); PluginFields::Instance().Refresh(); QTRY_VERIFY_WITH_TIMEOUT(Catalog.count(), 10000);
        const auto Fields = PluginFields::Instance().Fields(); QString Integer, Real, Demand, Delayed; int Plugins = 0;
        for (const auto& Field : Fields) if (Field.provider_ == ProviderKind::Wdx) {
            ++Plugins; if (Field.id_.endsWith(".0.0")) Integer = Field.id_; if (Field.id_.endsWith(".1.1")) Real = Field.id_;
            if (Field.id_.endsWith(".8.0")) Demand = Field.id_; if (Field.id_.endsWith(".6.0")) Delayed = Field.id_;
        }
        QCOMPARE(Plugins, 10); QVERIFY(!Integer.isEmpty());
        hc::workspace::TabState Tab; Tab.id_ = "wdx-columns"; Tab.location_ = {hc::storage::LocatorKind::Local, Directory.path().toStdString()};
        hc::workspace::PaneState Pane; Pane.tabs_ = {Tab}; auto* Panel = main_->findChild<CmpPanel*>("cmpLeftPanel"); Panel->RestoreWorkspace(Pane);
        auto* Model = static_cast<CFSModel*>(model(Panel)); QTRY_VERIFY(!Model->IsLoading() && Model->rowCount() >= 3);
        auto* View = static_cast<CGridView*>(Panel->GetFocusView(0)); auto Profile = DefaultProfile();
        Profile.columns_[1].fieldId_ = Integer; Profile.columns_[2].fieldId_ = Real; Profile.columns_[3].fieldId_ = Demand;
        Profile.columns_[4].fieldId_ = Delayed; Profile.columns_[4].widths_[0].hidden_ = false;
        ScriptDefinition Derived; Derived.title_ = "WDX derived"; Derived.dependencies_ = {Integer}; Derived.outputs_ = {{"next", "Next", FieldType::Integer}};
        Derived.source_ = "return function(item) return {next=item['" + Integer + "']+1} end"; Profile.scripts_.append(Derived);
        Profile.columns_[5].fieldId_ = ScriptPrefix(Derived) + "next"; Profile.columns_[5].widths_[0].hidden_ = false;
        Profile.sorts_ = {{Profile.columns_[5].id_, Qt::DescendingOrder}, {Profile.columns_[1].id_, Qt::AscendingOrder}};
        QVERIFY(View->ApplyColumnProfile(Profile));
        int Row = 0; for (; Row < Model->rowCount(); ++Row) if (Model->GetName(Model->index(Row, 0)) == "data.dll") break; QVERIFY(Row < Model->rowCount());
        QTRY_COMPARE(Model->index(Row, 1).data().toString(), QString("9007199254740993"));
        QTRY_COMPARE(Model->index(Row, 2).data().toString(), QString("표시값"));
        QCOMPARE(Model->index(Row, 2).data(CFsModelT::USR_ROLE_SORT).toDouble(), 32.5);
        QTRY_COMPARE(Model->index(Row, 4).data().toString(), QString("resolved"));
        QTRY_COMPARE(Model->index(Row, 5).data(CFsModelT::USR_ROLE_SORT).toLongLong(), qint64(9007199254740994LL));
        Model->index(Row, 3).data(); QTRY_COMPARE(Model->index(Row, 3).data(CFSModel::ColumnStateRole).toInt(), int(ValueState::OnDemand));
        Model->ResumeColumnQueries(); QTRY_COMPARE(Model->index(Row, 3).data().toString(), QString("explicit"));
        Settings.SetPlugins("wdx", {}); Catalog.clear(); PluginFields::Instance().Refresh(); QTRY_VERIFY(Catalog.count());
        Model->index(Row, 1).data(); QTRY_COMPARE(Model->index(Row, 1).data(CFSModel::ColumnStateRole).toInt(), int(ValueState::Unsupported));
        QVERIFY(IsSupportedProfile(View->ColumnProfile()));
    }

    void columnLuaUi()
    {
        using namespace hc::columns;
        const auto Workspace = main_->CaptureWorkspace(); const auto Restore = qScopeGuard([&] { main_->RestoreWorkspace(Workspace); });
        QTemporaryDir Directory; QImage Picture(64, 32, QImage::Format_RGBA8888); Picture.fill(Qt::red); QVERIFY(Picture.save(Directory.filePath("picture.png")));
        hc::workspace::TabState Tab; Tab.id_ = "columns-lua"; Tab.location_ = {hc::storage::LocatorKind::Local, Directory.path().toStdString()};
        hc::workspace::PaneState Pane; Pane.tabs_ = {Tab}; auto* Panel = main_->findChild<CmpPanel*>("cmpLeftPanel"); Panel->RestoreWorkspace(Pane);
        auto* Model = static_cast<CFSModel*>(model(Panel)); QTRY_VERIFY(!Model->IsLoading() && Model->rowCount() == 2);
        auto* View = static_cast<CGridView*>(Panel->GetFocusView(0)); auto Profile = DefaultProfile();
        ScriptDefinition Script; Script.title_ = "Dimensions"; Script.dependencies_ = {"file.imageWidth", "file.imageHeight"};
        Script.outputs_ = {{"area", "Area", FieldType::Integer}, {"label", "Label", FieldType::Text}};
        Script.source_ = "return function(item) local area=item['file.imageWidth']*item['file.imageHeight']; return {area=area, label=tostring(area)..' px'} end";
        Profile.scripts_.append(Script); Profile.columns_[1].fieldId_ = ScriptPrefix(Script) + "area"; Profile.columns_[2].fieldId_ = ScriptPrefix(Script) + "label";
        Profile.sorts_ = {{Profile.columns_[1].id_, Qt::DescendingOrder}}; QVERIFY(View->ApplyColumnProfile(Profile));
        const auto Cell = [&](int Column) { for (int Row = 0; Row < Model->rowCount(); ++Row) if (Model->GetName(Model->index(Row, 0)) == "picture.png") return Model->index(Row, Column); return QModelIndex{}; };
        QTRY_COMPARE(Cell(1).data(CFsModelT::USR_ROLE_SORT).toLongLong(), 2048); QTRY_COMPARE(Cell(2).data().toString(), QString("2048 px"));
        QCOMPARE(Model->PendingColumnQueries(), 0);
        Profile.scripts_[0].source_ = "return function(item) return {area=42, label='changed'} end"; QVERIFY(View->ApplyColumnProfile(Profile));
        QTRY_COMPARE(Cell(2).data().toString(), QString("changed")); QCOMPARE(Cell(1).data(CFsModelT::USR_ROLE_SORT).toLongLong(), 42);
        Profile.scripts_[0].outputs_[0].type_ = FieldType::Text; QVERIFY(View->ApplyColumnProfile(Profile));
        QTRY_COMPARE(Cell(1).data(CFSModel::ColumnStateRole).toInt(), int(ValueState::Error));
        Profile.scripts_[0].outputs_[0].type_ = FieldType::Integer; QVERIFY(View->ApplyColumnProfile(Profile));
        Model->CancelColumnQueries(); QCOMPARE(Cell(1).data(CFSModel::ColumnStateRole).toInt(), int(ValueState::Ready)); // Cached values survive cancellation.
        Model->ResumeColumnQueries(); QTRY_COMPARE(Cell(1).data(CFsModelT::USR_ROLE_SORT).toLongLong(), 42);
        QMainOpts Dialog(main_); Dialog.SetListView(View); Dialog.ShowListSettingsPage(); Dialog.show(); QCoreApplication::processEvents();
        auto* Editor = Dialog.findChild<QtColumnProfileEditor*>(); auto* Source = Editor->findChild<QPlainTextEdit*>("hc.columns.scriptSource"); QVERIFY(Source);
        Source->setPlainText("return ("); QVERIFY(!Editor->ValidateDraft()); Source->setPlainText(Profile.scripts_[0].source_); QVERIFY(Editor->ValidateDraft());
        Editor->findChild<QPushButton*>("hc.columns.addScript")->click(); QCOMPARE(Editor->Profile().scripts_.size(), 2); QVERIFY(Editor->ValidateDraft());
        Editor->findChild<QTabWidget*>("hc.columns.pages")->setCurrentIndex(3);
        QVERIFY(Dialog.grab().save(QCoreApplication::applicationDirPath() + "/columns-lua-editor.png")); Dialog.reject();
        const auto Saved = Panel->CaptureWorkspace(); Panel->RestoreWorkspace(Saved); Model = static_cast<CFSModel*>(model(Panel));
        QTRY_VERIFY(!Model->IsLoading() && Model->rowCount() == 2); QTRY_COMPARE(Cell(2).data().toString(), QString("changed"));
        auto Provider = std::make_shared<GatedProvider>(); Provider->inner_.Add(Provider->inner_.Root(), "virtual.txt", std::vector<std::byte>(13)); Provider->release_.set_value();
        View = static_cast<CGridView*>(Panel->GetFocusView(0)); Profile.scripts_[0].dependencies_ = {"file.size", "file.name"};
        Profile.scripts_[0].source_ = "return function(item) return {area=item['file.size'], label=item['file.name']} end";
        QVERIFY(View->ApplyColumnProfile(Profile)); Model->SetRoot(nsHC::TySpFileSystem(new GatedFs(Provider))); Model->SetCurrentPath("\\"); Model->Refresh();
        QTRY_VERIFY(!Model->IsLoading() && names(Model).contains("virtual.txt"));
        int VirtualRow = -1; for (int Row = 0; Row < Model->rowCount(); ++Row) if (Model->GetName(Model->index(Row, 0)) == "virtual.txt") VirtualRow = Row;
        QVERIFY(VirtualRow >= 0); QTRY_COMPARE(Model->index(VirtualRow, 2).data().toString(), QString("virtual.txt"));
        QCOMPARE(Model->index(VirtualRow, 1).data(CFsModelT::USR_ROLE_SORT).toLongLong(), 13);
    }
    void columnsLargeListingUi()
    {
        using namespace hc::columns;
        const auto Workspace = main_->CaptureWorkspace(); const auto Restore = qScopeGuard([&] { main_->RestoreWorkspace(Workspace); });
        auto Provider = std::make_shared<GatedProvider>();
        for (int Index = 0; Index < 100000; ++Index) Provider->inner_.Add(Provider->inner_.Root(), "item" + std::to_string(100000 - Index) + ".txt");
        Provider->release_.set_value(); auto* Panel = main_->findChild<CmpPanel*>("cmpLeftPanel");
        auto* Model = static_cast<CFSModel*>(model(Panel)); auto* View = static_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex()));
        QVERIFY(View->ApplyColumnProfile(DefaultProfile()));
        QElapsedTimer Clock; Clock.start(); Model->SetRoot(nsHC::TySpFileSystem(new GatedFs(Provider))); Model->SetCurrentPath("\\"); Model->Refresh();
        QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading() && Model->UsesIndexedOrder() && Model->EntryCount() == 100000, 120000); const auto LoadMs = Clock.elapsed();
        QCOMPARE(Model->rowCount(), 100001);
        QTRY_COMPARE_WITH_TIMEOUT(Model->GetName(Model->index(1, 0)), QString("item1.txt"), 120000);
        QCOMPARE(Model->GetName(Model->index(4097, 0)), QString("item4097.txt"));
        QCOMPARE(Model->GetName(Model->index(100000, 0)), QString("item100000.txt"));
        QVERIFY(!View->findChild<QWidget*>("hc.entries.pageBar"));
        auto Selection = Model->SelectAllEntries(); QVERIFY(Selection.HasValue()); QCOMPARE(Selection.Value()->Count(), 100000ULL);
        QCOMPARE(Selection.Value()->Provider(), Model->SnapshotProvider());
        QCOMPARE(Selection.Value()->Read(99999, 1).Value().entries_.front().name_, std::string("item100000.txt"));
        View->EnsureKeyboardFocusOnView(); QVERIFY(QApplication::focusWidget());
        // This scenario exercises the legacy whole-snapshot fallback. Ctrl+A
        // now defaults to compression, so explicitly unbind it for this check.
        {
            auto& Store = hc::settings::AppSettings(); const auto Previous = Store.Read(hc::settings::Section::Shortcuts);
            const auto RestoreKey = qScopeGuard([&] { Store.Put(hc::settings::Section::Shortcuts, Previous); TyStCommandMgr::GetInstance()->Refresh(); });
            auto Overrides = Previous.toObject(); Overrides["Ctrl+A"] = QJsonValue::Null; Store.Put(hc::settings::Section::Shortcuts, Overrides); TyStCommandMgr::GetInstance()->Refresh();
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_A, Qt::ControlModifier);
            QVERIFY(Model->property("hc.selectAllEntries").toBool());
        }
        View->BaseView()->deselectAll(); QVERIFY(!Model->property("hc.selectAllEntries").toBool());
        auto Profile = View->ColumnProfile(); Profile.sorts_ = {{Profile.columns_[0].id_, Qt::DescendingOrder}};
        Clock.restart(); QVERIFY(View->ApplyColumnProfile(Profile));
        QTRY_COMPARE_WITH_TIMEOUT(Model->GetName(Model->index(1, 0)), QString("item100000.txt"), 120000); const auto SortMs = Clock.elapsed();
        // A selection retains the old order and revisions after a new projection.
        QCOMPARE(Selection.Value()->Read(0, 1).Value().entries_.front().name_, std::string("item1.txt"));
        auto* Proxy = qobject_cast<CFSProxyModel*>(View->BaseView()->model()); QVERIFY(Proxy);
        Proxy->SetSearch("name:item99999.txt");
        QTRY_COMPARE_WITH_TIMEOUT(Model->EntryCount(), 1ULL, 120000);
        QCOMPARE(Model->GetName(Model->index(1, 0)), QString("item99999.txt")); QCOMPARE(Model->rowCount(), 2);
        Proxy->SetSearch("name:no-such-item"); QTRY_COMPARE_WITH_TIMEOUT(Model->EntryCount(), 0ULL, 120000);
        QCOMPARE(Model->rowCount(), 1);
        Proxy->SetSearch("item10000", "starts"); QTRY_COMPARE_WITH_TIMEOUT(Model->EntryCount(), 2ULL, 30000);
        Proxy->SetSearch("100000.txt", "ends"); QTRY_COMPARE_WITH_TIMEOUT(Model->EntryCount(), 1ULL, 30000);
        QCOMPARE(Model->GetName(Model->index(1, 0)), QString("item100000.txt"));
        Proxy->SetSearch("item100000", "contains"); QTRY_COMPARE_WITH_TIMEOUT(Model->EntryCount(), 1ULL, 30000);
        Proxy->SetSearch({}); QTRY_COMPARE_WITH_TIMEOUT(Model->EntryCount(), 100000ULL, 120000);
        const auto FocusName = [&] { auto Index = View->BaseView()->focusedRow().modelIndex(0); while (const auto* Adapter = qobject_cast<const QAbstractProxyModel*>(Index.model())) Index = Adapter->mapToSource(Index); return Index.isValid() ? Model->GetName(Index) : QString{}; };
        auto& Store = hc::settings::AppSettings(); const auto PreviousGeneral = Store.Read(hc::settings::Section::General);
        auto* Find = main_->findChild<QLineEdit*>("hc.mainFind"); QVERIFY(Find);
        const auto RestoreQuickFind = qScopeGuard([&] { QTest::keyClick(Find, Qt::Key_Escape); Store.Put(hc::settings::Section::General, PreviousGeneral); });
        auto General = PreviousGeneral.toObject(); General["quickFind"] = QJsonObject{{"enabled", true}, {"activation", "typing"}, {"scope", "panel"}, {"match", "starts"}, {"result", "cursor"}};
        QVERIFY(Store.Put(hc::settings::Section::General, General));
        TyStCommandMgr::GetInstance()->Bridge()->ExecuteCommand("hc.search.focus"); Find->clear(); View->EnsureKeyboardFocusOnView(); QTest::qWait(30);
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_I); QTRY_VERIFY(Find->hasFocus()); QTest::keyClicks(Find, "tem1");
        QTRY_COMPARE_WITH_TIMEOUT(FocusName(), QString("item100000.txt"), 30000);
        QTest::keyClick(Find, Qt::Key_Down); QTRY_COMPARE_WITH_TIMEOUT(FocusName(), QString("item19999.txt"), 30000);
        QCOMPARE(Model->rowCount(), 100001); QCOMPARE(Model->EntryCount(), 100000ULL);
        QTest::keyClick(Find, Qt::Key_Up); QTRY_COMPARE_WITH_TIMEOUT(FocusName(), QString("item100000.txt"), 30000);
        QCOMPARE(Model->rowCount(), 100001); QCOMPARE(Model->EntryCount(), 100000ULL);
        QTest::keyClick(Find, Qt::Key_Down); Find->setText("item90000.txt");
        QTRY_COMPARE_WITH_TIMEOUT(FocusName(), QString("item90000.txt"), 30000); QCOMPARE(Model->EntryCount(), 100000ULL);
        auto* Scroll = View->BaseView()->verticalScrollBar(); Clock.restart();
        for (int Index = 0; Index < 20; ++Index) { Scroll->setValue(Index % 2 ? Scroll->minimum() : Scroll->maximum()); QCoreApplication::processEvents(); }
        const auto ScrollMs = Clock.elapsed(); Clock.restart(); Model->CancelColumnQueries(); const auto CancelMs = Clock.elapsed();
        qInfo() << "100k continuous listing load/sort/20-scroll/cancel ms" << LoadMs << SortMs << ScrollMs << CancelMs;
        QCOMPARE(Model->PendingColumnQueries(), 0); QVERIFY(CancelMs < 500);
    }
    void columnWorkspaceSelectionUi()
    {
        using namespace hc::columns;
        auto& Theme = fm::style::ThemeManager::instance();
        const auto SavedDesign = Theme.design(); const auto SavedScheme = Theme.scheme();
        const auto SavedTone = Theme.darkTone();
        const auto RestoreTheme = qScopeGuard([&] { Theme.setDesign(SavedDesign); Theme.setScheme(SavedScheme); Theme.setDarkTone(SavedTone); });
        Theme.setDesign(fm::style::Design::Watercolor); Theme.setScheme(fm::style::ThemeManager::Scheme::Dark);
        QtColumnProfileEditor Editor;
        auto Profile = ReferenceProfile(); std::swap(Profile.columns_[1], Profile.columns_[3]);
        Editor.Load(Profile, {}); Editor.resize(1280, 760); Editor.show();
        QVERIFY(QTest::qWaitForWindowExposed(&Editor));
        auto* Pages = Editor.findChild<QTabWidget*>("hc.columns.pages"); QVERIFY(Pages); QCOMPARE(Pages->count(), 4);
        auto* List = Editor.findChild<QListWidget*>("hc.columns.list"); QVERIFY(List);
        auto* Grid = Editor.findChild<Qtitan::Grid*>("hc.columns.previewGrid"); QVERIFY(Grid);
        auto* Native = Grid->view<Qtitan::GridBandedTableView>();
        const auto HeaderFor = [&](const QUuid& Id) {
            Qtitan::GridColumnBase* Result = nullptr;
            for (int Index = 0; Index < Editor.Profile().columns_.size(); ++Index) {
                auto* Column = Native->getColumnByModelColumn(Index);
                if (Column && Column->property("hc.columnId").toUuid() == Id) Result = Column;
            }
            return Result;
        };
        const auto ClickHeader = [&](const QUuid& Id) {
            auto* Column = HeaderFor(Id); QVERIFY(Column);
            Qtitan::ColumnClickEventArgs Args(Column); Native->columnClicked(&Args);
        };
        const auto SizeId = Editor.Profile().columns_[1].id_;
        ClickHeader(SizeId); QTRY_COMPARE(List->currentRow(), 1);
        auto* Title = Editor.findChild<QLineEdit*>("hc.columns.title"); QCOMPARE(Title->text(), Editor.Profile().columns_[1].title_);
        QVERIFY(Editor.findChild<QComboBox*>("hc.columns.sizeFormat")->isVisible());
        QVERIFY(!Editor.findChild<QComboBox*>("hc.columns.dateFormat")->isVisible());
        Editor.findChild<QPushButton*>("hc.columns.duplicate")->click();
        const auto CopyId = Editor.Profile().columns_[2].id_; QVERIFY(CopyId != SizeId);
        Editor.findChild<QPushButton*>("hc.columns.down")->click();
        QCOMPARE(Editor.Profile().columns_[3].id_, CopyId);
        ClickHeader(SizeId); QTRY_COMPARE(List->currentRow(), 1);
        ClickHeader(CopyId); QTRY_COMPARE(List->currentRow(), 3);
        Title->setFocus(); Title->selectAll(); QTest::keyClicks(Title, "Preview size");
        QCOMPARE(HeaderFor(CopyId)->caption(), QString("Preview size"));
        QCOMPARE(Editor.Profile().columns_[1].id_, SizeId); QVERIFY(HeaderFor(SizeId)->caption() != "Preview size");
        List->item(3)->setCheckState(Qt::Unchecked); QVERIFY(!HeaderFor(CopyId)->isVisible());
        ClickHeader(SizeId); QTRY_COMPARE(List->currentRow(), 1);
        List->setCurrentRow(3); QCOMPARE(Title->text(), QString("Preview size"));
        QVERIFY(!Editor.findChild<QCheckBox*>("hc.columns.visible")->isChecked());
        List->item(3)->setCheckState(Qt::Checked); QVERIFY(HeaderFor(CopyId)->isVisible());
        Editor.findChild<QPushButton*>("hc.columns.remove")->click();
        QVERIFY(!HeaderFor(CopyId)); QCOMPARE(List->count(), Profile.columns_.size());
        // The name binding stays at model column zero even when moved visually.
        List->setCurrentRow(0); const auto NameId = Editor.Profile().primaryNameId_;
        Editor.findChild<QPushButton*>("hc.columns.down")->click();
        ClickHeader(SizeId); QTRY_COMPARE(List->currentRow(), 0);
        ClickHeader(NameId); QTRY_COMPARE(List->currentRow(), 1);
        // Header editing also works with an empty selection as preview source.
        Editor.SetSamples({}, {}); Editor.findChild<QComboBox*>("hc.columns.sampleMode")->setCurrentIndex(2);
        QCOMPARE(Editor.findChild<QTableWidget*>("hc.columns.preview")->rowCount(), 0);
        ClickHeader(SizeId); QTRY_COMPARE(List->currentRow(), 0);
        Editor.Load(Profile, {}); Editor.findChild<QComboBox*>("hc.columns.sampleMode")->setCurrentIndex(0);
        List->setCurrentRow(1); QTest::qWait(150);
        auto* Inspector = Editor.findChild<QScrollArea*>("hc.columns.inspectorScroll"); QVERIFY(Inspector);
        QCOMPARE(Inspector->verticalScrollBar()->maximum(), 0);
        QVERIFY(Editor.grab().save(QCoreApplication::applicationDirPath() + "/columns-workspace.png"));
        Editor.resize(1100, 700); QTest::qWait(150);
        QVERIFY(Editor.grab().save(QCoreApplication::applicationDirPath() + "/columns-workspace-compact.png"));
        QVERIFY(Editor.ValidateDraft());
        Editor.hide();
        QMainOpts Dialog(main_);
        Dialog.SetListView(static_cast<CGridView*>(panels_.front()->GetFocusView(0)));
        Dialog.ShowListSettingsPage(); Dialog.show(); QTest::qWait(100); Dialog.resize(1280, 760);
        auto* Embedded = Dialog.findChild<QtColumnProfileEditor*>(); QVERIFY(Embedded);
        Embedded->Load(Profile, {}); Embedded->findChild<QListWidget*>("hc.columns.list")->setCurrentRow(1);
        QTest::qWait(150);
        QVERIFY(Dialog.grab().save(QCoreApplication::applicationDirPath() + "/columns-workspace-settings.png"));
        auto* Viewport = Dialog.findChild<QScrollArea*>("hc.settings.columnsScroll"); QVERIFY(Viewport);
        QCOMPARE(Viewport->verticalScrollBar()->maximum(), 0); QCOMPARE(Viewport->horizontalScrollBar()->maximum(), 0);
        auto* EmbeddedInspector = Embedded->findChild<QScrollArea*>("hc.columns.inspectorScroll"); QVERIFY(EmbeddedInspector);
        QCOMPARE(EmbeddedInspector->verticalScrollBar()->maximum(), 0); QCOMPARE(EmbeddedInspector->horizontalScrollBar()->maximum(), 0);
        Dialog.reject();
    }
    void columnPreviewAndScopesUi()
    {
        using namespace hc::columns;
        auto& Settings = hc::settings::AppSettings(); const auto Original = Settings.Object(); const auto Workspace = main_->CaptureWorkspace();
        const auto Restore = qScopeGuard([&] { Settings.DiscardEdit(); Settings.Replace(Original); Settings.Commit(); main_->RestoreWorkspace(Workspace); main_->SaveWorkspace(); });
        QTemporaryDir Directory; QImage Image(16, 8, QImage::Format_RGBA8888); Image.fill(Qt::blue); QVERIFY(Image.save(Directory.filePath("picture.png")));
        { QFile File(Directory.filePath("a.txt")); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("sample"); }
        auto* Left = main_->findChild<CmpPanel*>("cmpLeftPanel"); auto* Right = main_->findChild<CmpPanel*>("cmpRightPanel");
        hc::workspace::TabState Tab; Tab.id_ = "columns-preview"; Tab.location_ = {hc::storage::LocatorKind::Local, Directory.path().toStdString()};
        hc::workspace::PaneState Pane; Pane.tabs_ = {Tab}; Left->RestoreWorkspace(Pane); Pane.tabs_[0].id_ += "-right"; Right->RestoreWorkspace(Pane);
        QTRY_VERIFY(!model(Left)->IsLoading() && names(model(Left)).contains("picture.png")); QTRY_VERIFY(!model(Right)->IsLoading());
        auto* View = static_cast<CGridView*>(Left->GetFocusView(0)); auto* Other = static_cast<CGridView*>(Right->GetFocusView(0));
        auto* Model = static_cast<CFSModel*>(model(Left)); auto Profile = DefaultProfile();
        ScriptDefinition Script; Script.title_ = "Pixels"; Script.outputs_ = {{"area", "Area", FieldType::Integer}};
        Script.dependencies_ = {"file.imageWidth", "file.imageHeight"}; Script.source_ = "return function(item) return {area=item['file.imageWidth']*item['file.imageHeight']} end";
        Profile.scripts_.append(Script); Profile.columns_[1].fieldId_ = ScriptPrefix(Script) + "area"; Profile.columns_[2].fieldId_ = "file.imageWidth";
        QVERIFY(View->ApplyColumnProfile(Profile)); int Row = -1; for (int Index = 0; Index < Model->rowCount(); ++Index) if (Model->GetName(Model->index(Index, 0)) == "picture.png") Row = Index;
        const auto Key = hc::workspace::SelectionKey(*Model->GetFileInfo(Model->index(Row, 0))->storageEntry_); View->RestoreSelection({Key}, Key);
        QMainOpts Dialog(main_); Dialog.SetListView(View); Dialog.ShowListSettingsPage(); Dialog.show(); QCoreApplication::processEvents();
        auto* Editor = Dialog.findChild<QtColumnProfileEditor*>(); auto* Preview = Editor->findChild<QTableWidget*>("hc.columns.preview"); auto* Mode = Editor->findChild<QComboBox*>("hc.columns.sampleMode");
        Mode->setCurrentIndex(2); QCOMPARE(Preview->rowCount(), 1); QVERIFY(Preview->item(0, 1)->text() != "128");
        auto* Calculate = Editor->findChild<QPushButton*>("hc.columns.calculate"); Calculate->click();
        Editor->findChild<QPushButton*>("hc.columns.stopPreview")->click(); QTest::qWait(100);
        QVERIFY(Preview->item(0, 1)->text() != "128"); // Cancelled work cannot populate the draft afterwards.
        Calculate->click();
        QTRY_COMPARE(Preview->item(0, 1)->text(), QString("128")); QCOMPARE(Preview->item(0, 2)->text(), QString("16"));
        Preview->setCurrentCell(0, 1); QVERIFY(Editor->findChild<QLabel*>("hc.columns.valueDetail")->text().contains("128"));
        Editor->findChild<QListWidget*>("hc.columns.list")->setCurrentRow(1); Editor->findChild<QCheckBox*>("hc.columns.plusSign")->setChecked(true);
        QCOMPARE(Preview->item(0, 1)->text(), QString("+128")); // A format edit keeps ready values and starts no new query.
        auto* Search = Editor->findChild<QLineEdit*>("hc.columns.search"); Search->setText("imageWidth");
        auto* Field = Editor->findChild<QComboBox*>("hc.columns.field"); QVERIFY(Field->findData("file.imageWidth") >= 0); QVERIFY(Field->findData("file.company") < 0); Search->clear();
        Mode->setCurrentIndex(3); Editor->findChild<QLineEdit*>("hc.columns.samplePath")->setText(Directory.filePath("picture.png"));
        Editor->findChild<QPushButton*>("hc.columns.readSample")->click(); QTRY_COMPARE(Preview->rowCount(), 1); QTRY_COMPARE(Preview->item(0, 0)->text(), QString("picture.png"));
        Calculate->click(); QTRY_COMPARE(Preview->item(0, 1)->text(), QString("+128")); Editor->findChild<QPushButton*>("hc.columns.stopPreview")->click();
        Editor->findChild<QComboBox*>("hc.columns.scope")->setCurrentIndex(2); QVERIFY(Dialog.SaveSettings());
        QVERIFY(View->ColumnProfile() == Other->ColumnProfile()); QCOMPARE(View->CaptureSelection(), QStringList{Key});
        const auto Applied = View->ColumnProfile(); Editor->findChild<QComboBox*>("hc.columns.scope")->setCurrentIndex(3);
        auto* Title = Editor->findChild<QLineEdit*>("hc.columns.title"); Title->setFocus(); Title->selectAll(); QTest::keyClicks(Title, "New tab only");
        QVERIFY(Dialog.SaveSettings()); QVERIFY(View->ColumnProfile() == Applied); QVERIFY(Other->ColumnProfile() == Applied);
        const auto Default = DecodeProfile(Settings.Read(hc::settings::Section::Columns).toObject()["ColumnsV2/Defaults/" + View->ColumnWidths()->PaneKey()].toObject());
        QVERIFY(Default); QCOMPARE(Default->columns_[1].title_, QString("New tab only"));
        auto* Native = Editor->findChild<Qtitan::Grid*>("hc.columns.previewGrid")->view<Qtitan::GridBandedTableView>();
        QTRY_COMPARE(Native->modelController()->getGridRowCount(), 1);
        QCOMPARE(Native->getRow(0).modelIndex(0).data().toString(), QString("picture.png"));
        QCOMPARE(Native->getRow(0).modelIndex(1).data().toString(), QString("+128"));
        QTest::qWait(150); // Let the native grid finish its deferred layout before capturing pixels.
        QModelRoleData Roles[]{QModelRoleData(Qt::DecorationRole)}; Qtitan::GridModelDataBinding::ModelRoleData Span(Roles);
        QVERIFY(Native->modelController()->getModelMultiValue(Native->getRow(0).modelIndex(0), Span));
        QCOMPARE(Roles[0].data().value<QIcon>().cacheKey(), Preview->item(0, 0)->icon().cacheKey());
        qInfo() << "Column dialog / minimum / preferred:" << Dialog.size() << Dialog.minimumSizeHint() << Dialog.sizeHint();
        QVERIFY(Dialog.height() <= Dialog.screen()->availableGeometry().height());
        QVERIFY(Dialog.grab().save(QCoreApplication::applicationDirPath() + "/columns-stage7-preview.png")); Dialog.reject();
        QMainOpts Failure(main_); Failure.SetListView(View); Failure.ShowListSettingsPage(); Failure.show(); QCoreApplication::processEvents();
        auto* Draft = Failure.findChild<QtColumnProfileEditor*>(); Draft->findChild<QPushButton*>("hc.columns.add")->click();
        QLockFile Lock(Settings.FileName() + ".lock"); QVERIFY(Lock.tryLock());
        QTimer Close; connect(&Close, &QTimer::timeout, &Failure, [] { for (auto* Widget : QApplication::topLevelWidgets()) if (auto* Message = qobject_cast<QMessageBox*>(Widget)) Message->reject(); }); Close.start(10);
        QVERIFY(!Failure.SaveSettings()); Close.stop(); Lock.unlock(); QVERIFY(View->ColumnProfile() == Applied); QVERIFY(Draft->Dirty()); Failure.reject();
    }
    void columnTemplatesAndAsyncUi()
    {
        using namespace hc::columns;
        const auto Workspace = main_->CaptureWorkspace();
        const auto Restore = qScopeGuard([&] { main_->RestoreWorkspace(Workspace); });
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        QImage Picture(64, 32, QImage::Format_RGBA8888); Picture.fill(Qt::red); QVERIFY(Picture.save(Directory.filePath("picture.png")));
        { QFile File(Directory.filePath("text.txt")); QVERIFY(File.open(QIODevice::WriteOnly)); File.write(QByteArray(8192, 'a')); }
        QVERIFY(QDir(Directory.path()).mkdir("folder"));
        hc::workspace::TabState Tab; Tab.id_ = "columns-stage3"; Tab.location_ = {hc::storage::LocatorKind::Local, Directory.path().toStdString()};
        hc::workspace::PaneState Pane; Pane.tabs_ = {Tab}; auto* Panel = main_->findChild<CmpPanel*>("cmpLeftPanel"); Panel->RestoreWorkspace(Pane);
        auto* Model = static_cast<CFSModel*>(model(Panel)); QTRY_VERIFY(!Model->IsLoading() && Model->rowCount() == 4);
        auto* View = static_cast<CGridView*>(Panel->GetFocusView(0));
        auto Profile = DefaultProfile();
        const auto Add = [&](const QString& Field, const QString& Title) {
            ColumnDefinition Column; Column.fieldId_ = Field; Column.title_ = Title;
            Column.widths_.fill({hc::view::WidthUnit::Pixels, hc::view::WidthMode::Flexible, 180}); Profile.columns_.append(Column); return Column.id_;
        };
        const auto Composite = Add({}, "Template"); Profile.columns_.last().template_ = "${file.stem} / ${file.size}";
        Profile.columns_.last().sortField_ = "file.size"; Profile.columns_.last().format_.size_ = SizeFormat::KiB;
        const auto Dimensions = Add("file.imageDimensions", "Dimensions");
        const auto Allocated = Add("file.allocatedSize", "Allocated");
        Profile.sorts_ = {{Allocated, Qt::DescendingOrder}, {Profile.primaryNameId_, Qt::AscendingOrder}};
        QVERIFY(View->ApplyColumnProfile(Profile));
        const auto IndexOf = [&](const QUuid& Id) { for (int Col = 0; Col < Model->columnCount(); ++Col) if (View->ColumnWidths()->ColumnId(Col) == IdText(Id)) return Col; return -1; };
        const auto RowOf = [&](const QString& Name) { for (int Row = 0; Row < Model->rowCount(); ++Row) if (Model->GetName(Model->index(Row, 0)) == Name) return Row; return -1; };
        const auto ImageCell = [&] { return Model->index(RowOf("picture.png"), IndexOf(Dimensions)); };
        ImageCell().data(); QTRY_COMPARE(ImageCell().data().toString(), QString("64 × 32"));
        QCOMPARE(ImageCell().data(CFSModel::ColumnStateRole).toInt(), int(ValueState::Ready));
        QTRY_COMPARE(Model->PendingColumnQueries(), 0);
        QTRY_VERIFY(Model->index(RowOf("text.txt"), 0).data(CFSModel::GridSortRole).toInt() != Model->index(RowOf("picture.png"), 0).data(CFSModel::GridSortRole).toInt());
        QCOMPARE(View->ColumnProfile().sorts_.size(), 2);
        QCOMPARE(Model->index(RowOf("text.txt"), IndexOf(Allocated)).data(CFSModel::ColumnStateRole).toInt(), int(ValueState::Ready));
        const auto CompositeCell = Model->index(RowOf("text.txt"), IndexOf(Composite));
        QVERIFY(CompositeCell.data().toString().startsWith("text / ")); QCOMPARE(CompositeCell.data(CFsModelT::USR_ROLE_SORT).toULongLong(), quint64(8192));
        const auto First = View->BaseView()->getRow(0).modelIndex(0); QCOMPARE(First.data().toString(), QString(".."));
        QCOMPARE(View->BaseView()->getRow(1).modelIndex(0).data().toString(), QString("folder"));
        QTRY_COMPARE(View->BaseView()->getRow(2).modelIndex(0).data().toString(), QString("text.txt"));
        QVERIFY(main_->grab().save(QCoreApplication::applicationDirPath() + "/columns-stage3-panel.png"));
        QMainOpts Dialog(main_); Dialog.SetListView(View); Dialog.ShowListSettingsPage(); Dialog.show(); QCoreApplication::processEvents();
        auto* Editor = Dialog.findChild<QtColumnProfileEditor*>(); QVERIFY(Editor);
        int TemplateRow = -1; for (int Index = 0; Index < Editor->Profile().columns_.size(); ++Index) if (Editor->Profile().columns_[Index].id_ == Composite) TemplateRow = Index;
        Editor->findChild<QListWidget*>("hc.columns.list")->setCurrentRow(TemplateRow);
        QCOMPARE(Editor->findChild<QListWidget*>("hc.columns.sortRules")->count(), 2);
        Editor->findChild<QPushButton*>("hc.columns.addSort")->click(); QCOMPARE(Editor->Profile().sorts_.size(), 3);
        const auto LastSort = Editor->Profile().sorts_.last();
        Editor->findChild<QPushButton*>("hc.columns.sortUp")->click(); QCOMPARE(Editor->Profile().sorts_[1], LastSort);
        Editor->findChild<QPushButton*>("hc.columns.sortDown")->click(); QCOMPARE(Editor->Profile().sorts_.last(), LastSort);
        QVERIFY(Editor->findChild<QComboBox*>("hc.columns.sizeFormat")->isEnabled());
        QVERIFY(Editor->findChild<QComboBox*>("hc.columns.dateFormat")->isEnabled());
        auto* Expression = Editor->findChild<QLineEdit*>("hc.columns.template"); QVERIFY(Expression->isEnabled());
        Expression->setText("${file.unknown}"); QVERIFY(!Editor->ValidateDraft());
        Expression->setText("${file.name} · ${file.size}"); QVERIFY(Editor->ValidateDraft());
        QVERIFY(Dialog.grab().save(QCoreApplication::applicationDirPath() + "/columns-stage3-editor.png")); Dialog.reject();
        QSignalSpy Refreshed(Model, &CFsModelT::sigRefreshCompleted);
        Picture = QImage(128, 96, QImage::Format_RGBA8888); Picture.fill(Qt::blue); QVERIFY(Picture.save(Directory.filePath("picture.png")));
        Model->Refresh(); QTRY_VERIFY(Refreshed.count()); ImageCell().data(); QTRY_COMPARE(ImageCell().data().toString(), QString("128 × 96"));
        auto Version = ReadNativeFields(qEnvironmentVariable("WINDIR") + "/System32/kernel32.dll", "version", {});
        QCOMPARE(Version["file.fileVersion"].State(), ValueState::Ready); QVERIFY(!Version["file.fileVersion"].Raw().toString().isEmpty());
        const auto Header = ReadNativeFields(Directory.filePath("picture.png"), "image", {});
        QCOMPARE(Header["file.imageBitDepth"].Raw().toULongLong(), quint64(32));
        Profile = View->ColumnProfile(); Add("file.fileVersion", "Version"); QVERIFY(View->ApplyColumnProfile(Profile));
        Model->CancelColumnQueries(); QCOMPARE(Model->index(RowOf("picture.png"), Model->columnCount() - 1).data(CFSModel::ColumnStateRole).toInt(), int(ValueState::OnDemand));
        Model->ResumeColumnQueries(); Model->index(RowOf("picture.png"), Model->columnCount() - 1).data();
        QTRY_COMPARE(Model->PendingColumnQueries(), 0);
    }

    void listLayoutSettingsUi()
    {
        using namespace hc::columns;
        auto& Settings = hc::settings::AppSettings(); const auto Original = Settings.Object();
        const auto Workspace = main_->CaptureWorkspace();
        const auto Restore = qScopeGuard([&] { Settings.DiscardEdit(); Settings.Replace(Original); Settings.Commit(); main_->RestoreWorkspace(Workspace); main_->SaveWorkspace(); });
        QVERIFY(Settings.Put(hc::settings::Section::Columns, QJsonObject{}));
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        for (const auto& Item : QList<QPair<QString, int>>{{"a.txt", 2}, {"b.txt", 10}, {"archive.tar.gz", 1536}}) {
            QFile File(Directory.filePath(Item.first)); QVERIFY(File.open(QIODevice::WriteOnly)); File.write(QByteArray(Item.second, 'x'));
        }
        hc::workspace::TabState Tab; Tab.id_ = "column-profile-test"; Tab.location_ = {hc::storage::LocatorKind::Local, Directory.path().toStdString()};
        hc::workspace::PaneState Pane; Pane.tabs_ = {Tab};
        auto* Left = main_->findChild<CmpPanel*>("cmpLeftPanel"); Left->RestoreWorkspace(Pane);
        QTRY_VERIFY(!model(Left)->IsLoading() && names(model(Left)).contains("a.txt"));
        auto* View = static_cast<CGridView*>(Left->GetFocusView(0)); auto* Adapter = View->ColumnWidths(); auto* Native = View->BaseView();
        const auto Before = View->CaptureWorkspaceView();
        const auto SetText = [](QLineEdit* Edit, const QString& Text) { Edit->setFocus(); Edit->selectAll(); QTest::keyClicks(Edit, Text); };
        QMainOpts Dialog(main_); Dialog.SetListView(View); Dialog.ShowListSettingsPage(); Dialog.show(); QCoreApplication::processEvents();
        auto* Editor = Dialog.findChild<QtColumnProfileEditor*>(); QVERIFY(Editor);
        auto* List = Editor->findChild<QListWidget*>("hc.columns.list");
        auto* Field = Editor->findChild<QComboBox*>("hc.columns.field");
        QCOMPARE(List->count(), 8); QVERIFY(!Dialog.findChild<QPushButton*>("hc.settings.editListAppearance"));
        const auto OriginalProfile = Editor->Profile();
        const auto OriginalLibrarySize = Editor->Library().size();
        Editor->findChild<QPushButton*>("hc.columns.cloneProfile")->click();
        QVERIFY(Editor->Profile().id_ != OriginalProfile.id_);
        QCOMPARE(Editor->Library().size(), OriginalLibrarySize + 1);
        for (int Index = 0; Index < OriginalProfile.columns_.size(); ++Index)
            QVERIFY(Editor->Profile().columns_[Index].id_ != OriginalProfile.columns_[Index].id_);
        List->setCurrentRow(1); Editor->findChild<QPushButton*>("hc.columns.duplicate")->click();
        QCOMPARE(List->count(), 9);
        Editor->findChild<QComboBox*>("hc.columns.sizeFormat")->setCurrentIndex(1);
        Editor->findChild<QSpinBox*>("hc.columns.precision")->setValue(1);
        SetText(Editor->findChild<QLineEdit*>("hc.columns.title"), "Adaptive size");
        const auto SizeId = Editor->Profile().columns_[1].id_;
        const auto FormattedId = Editor->Profile().columns_[2].id_;
        QVERIFY(SizeId != FormattedId);
        List->setCurrentRow(0); Field->setCurrentIndex(Field->findData("file.stem"));
        Editor->findChild<QPushButton*>("hc.columns.down")->click();
        QCOMPARE(Editor->Profile().primaryNameId_, Editor->Profile().columns_[1].id_);
        Editor->findChild<QPushButton*>("hc.columns.add")->click(); Field->setCurrentIndex(Field->findData("file.path"));
        Editor->findChild<QComboBox*>("hc.columns.unit")->setCurrentIndex(0);
        Editor->findChild<QDoubleSpinBox*>("hc.columns.width")->setValue(220);
        Editor->findChild<QComboBox*>("hc.columns.layout")->setCurrentIndex(2);
        QCOMPARE(Editor->findChild<QTableWidget*>("hc.columns.preview")->rowCount(), 3);
        Editor->findChild<QComboBox*>("hc.columns.scope")->setCurrentIndex(1);
        QCOMPARE(model(Left)->columnCount(), 8); QCOMPARE(View->CaptureWorkspaceView(), Before);
        QVERIFY(Editor->ValidateDraft()); QVERIFY(Dialog.SaveSettings());
        QCOMPARE(model(Left)->columnCount(), 10); QCOMPARE(Adapter->LayoutMode(), QString("name-last"));
        const auto Applied = View->ColumnProfile();
        const auto StoredProfiles = Settings.Read(hc::settings::Section::Columns).toObject()["ColumnsV2/Profiles"].toObject();
        QCOMPARE(StoredProfiles.size(), OriginalLibrarySize + 1); QVERIFY(StoredProfiles.contains(IdText(Applied.id_)));
        SetText(Editor->findChild<QLineEdit*>("hc.columns.title"), "Cancelled title"); Dialog.reject();
        QVERIFY(View->ColumnProfile() == Applied);
        const auto ModelColumn = [&](const QUuid& Id) { for (int Index = 0; Index < model(Left)->columnCount(); ++Index) if (Adapter->ColumnId(Index) == IdText(Id)) return Index; return -1; };
        const auto SizeIndex = ModelColumn(SizeId), FormattedIndex = ModelColumn(FormattedId); QVERIFY(SizeIndex >= 0 && FormattedIndex >= 0);
        int FileRow = -1;
        for (int Row = 0; Row < model(Left)->rowCount(); ++Row) if (model(Left)->GetName(model(Left)->index(Row, 0)) == "archive.tar.gz") FileRow = Row;
        QVERIFY(FileRow >= 0);
        QCOMPARE(model(Left)->index(FileRow, 0).data().toString(), QString("archive.tar"));
        QCOMPARE(model(Left)->index(FileRow, 0).data(Qt::EditRole).toString(), QString("archive.tar.gz"));
        QCOMPARE(model(Left)->index(FileRow, SizeIndex).data().toString(), QString("1536"));
        QVERIFY(model(Left)->index(FileRow, FormattedIndex).data().toString().endsWith("KiB"));
        QCOMPARE(model(Left)->index(FileRow, SizeIndex).data(CFsModelT::USR_ROLE_SORT), model(Left)->index(FileRow, FormattedIndex).data(CFsModelT::USR_ROLE_SORT));
        const auto Key = hc::workspace::SelectionKey(*model(Left)->GetFileInfo(model(Left)->index(FileRow, 0))->storageEntry_);
        View->RestoreSelection({Key}, Key);
        QVERIFY(View->ApplyColumnProfile(View->ColumnProfile()));
        QCOMPARE(View->CaptureSelection(), QStringList{Key}); QCOMPARE(View->CaptureFocus(), Key);
        auto SortedProfile = View->ColumnProfile();
        SortedProfile.sorts_ = {{QUuid(Adapter->ColumnId(FormattedIndex)), Qt::AscendingOrder}};
        QVERIFY(View->ApplyColumnProfile(SortedProfile));
        QCoreApplication::processEvents();
        QList<quint64> Sizes;
        for (int Row = 0; Row < Native->getRowCount(); ++Row) {
            const auto Value = Native->getRow(Row).modelIndex(FormattedIndex).data(CFsModelT::USR_ROLE_SORT);
            if (Value.isValid()) Sizes.append(Value.toULongLong());
        }
        QCOMPARE(Sizes, (QList<quint64>{2, 10, 1536}));
        auto Unsorted = SortedProfile; Unsorted.sorts_.clear();
        QVERIFY(View->ApplyColumnProfile(Unsorted)); QCOMPARE(Native->modelController()->sortRules()->sortRulesCount(), 0);
        QVERIFY(View->ApplyColumnProfile(SortedProfile));
        for (const auto& Mode : {QString("details"), QString("name-first"), QString("name-last")}) {
            QVERIFY(Adapter->SetLayoutMode(Mode));
            QTRY_COMPARE(static_cast<Qtitan::GridBandedTableColumn*>(Native->getColumnByModelColumn(0))->rowIndex(), Mode == "name-last" ? 1 : 0);
            QVERIFY(main_->grab().save(QCoreApplication::applicationDirPath() + "/custom-columns-" + Mode + ".png"));
        }
        QMainOpts Invalid(main_); Invalid.SetListView(View); Invalid.ShowListSettingsPage(); Invalid.show(); QCoreApplication::processEvents();
        auto* Draft = Invalid.findChild<QtColumnProfileEditor*>();
        Draft->findChild<QComboBox*>("hc.columns.unit")->setCurrentIndex(1);
        Draft->findChild<QDoubleSpinBox*>("hc.columns.width")->setValue(101);
        QVERIFY(!Invalid.SaveSettings()); QVERIFY(!Draft->findChild<QLabel*>("hc.columns.error")->text().isEmpty()); Invalid.reject();
        // New tabs follow the saved panel default even if this tab changes later.
        auto General = Settings.Read(hc::settings::Section::General).toObject();
        General["filePanel"] = QJsonObject{{"version", 1}, {"defaultMode", "two-line"}, {"nameBelow", true}};
        QVERIFY(Settings.Put(hc::settings::Section::General, General));
        QVERIFY(View->ApplyColumnProfile(DefaultProfile()));
        Left->AddTab(true); QCOMPARE(static_cast<CGridView*>(Left->GetFocusView(1))->BaseModel()->columnCount(), 10);
        QVERIFY(View->ApplyColumnProfile(SortedProfile));
        QVERIFY(Adapter->SetLayoutMode("name-last"));
        QVERIFY(static_cast<CGridView*>(Left->GetFocusView(1))->SetListMode("inherit"));
        QVERIFY(main_->SaveWorkspace());
        QProcess Restart; auto Environment = QProcessEnvironment::systemEnvironment();
        Environment.insert("HC_LIST_LAYOUT_RESTART", "1"); Environment.insert("HC_TEST_CONFIG_DIR", QFileInfo(Settings.FileName()).absolutePath()); Restart.setProcessEnvironment(Environment);
        Restart.start(QCoreApplication::applicationFilePath(), {"listLayoutRestartUi", "-o", "list-layout-restart.txt,txt"});
        QVERIFY(Restart.waitForStarted(5000)); QTRY_VERIFY_WITH_TIMEOUT(Restart.state() == QProcess::NotRunning, 20000);
        QCOMPARE(Restart.exitCode(), 0);
        Left->CloseTab();
        const QJsonObject Future{{"version", 99}, {"mode", "future"}, {"profiles", QJsonObject{}}};
        View->RestoreWorkspaceView({{WorkspaceKey, Future}}); QCOMPARE(Adapter->WorkspaceLayout(), Future); QVERIFY(!Adapter->Writable());
    }

    void workspaceRestoreUi()
    {
        using namespace hc::workspace;
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        for (const auto& Name : {"alpha.txt", "beta.txt"}) { QFile File(Directory.filePath(Name)); QVERIFY(File.open(QIODevice::WriteOnly)); File.write(Name); }
        auto* Panel = panels_.front();
        TabState Tab; Tab.id_ = "m7-active"; Tab.location_ = {hc::storage::LocatorKind::Local, Directory.path().toStdString()};
        Tab.view_ = {{"hiddenSystem", true}};
        auto Missing = Tab; Missing.id_ = "m7-missing"; Missing.location_ = {hc::storage::LocatorKind::Plugin, "wfx://missing/saved-folder"}; Missing.provider_ = Directory.filePath("removed.json");
        PaneState Pane; Pane.tabs_ = {Tab, Missing}; Panel->RestoreWorkspace(Pane);
        auto* Model = static_cast<CFSModel*>(model(Panel));
        QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading() && names(Model).contains("alpha.txt"), 10000);
        auto* View = qobject_cast<CGridView*>(Panel->GetFocusView(0)); QVERIFY(View);
        auto* Inactive = qobject_cast<CGridView*>(Panel->GetFocusView(1)); QVERIFY(Inactive);
        QVERIFY(!Inactive->BaseModel()->GetRoot()); // Lazy restore does not launch inactive providers.
        hc::storage::EntrySnapshot Selected;
        for (int Row = 0; Row < Model->rowCount(); ++Row) {
            const auto Item = Model->GetFileInfo(Model->index(Row, 0));
            if (Item && Item->Name_ == "beta.txt") Selected = *Item->storageEntry_;
        }
        const auto Key = SelectionKey(Selected); View->RestoreSelection({Key}, Key);
        auto Policy = View->ColumnWidths()->Policies().value(View->ColumnWidths()->ColumnId(0));
        Policy.value_ = 35; Policy.unit_ = hc::view::WidthUnit::Percent;
        QVERIFY(View->ColumnWidths()->SetPolicy(View->ColumnWidths()->ColumnId(0), Policy));
        auto* Column = static_cast<Qtitan::GridTableColumn*>(View->BaseView()->getColumnByModelColumn(0));
        Column->setSortOrder(Qtitan::SortDescending, true);
        const auto Saved = Panel->CaptureWorkspace(); QCOMPARE(Saved.tabs_[0].selection_, QStringList{Key});
        QPointer<CGridView> OldGrid = View;
        Panel->OpenQuickView(0, {}); QPointer<CViewT> OldPreview = Panel->GetFocusView(0);
        Panel->RestoreWorkspace(Saved);
        QVERIFY(OldGrid.isNull()); QTRY_VERIFY(OldPreview.isNull());
        Model = static_cast<CFSModel*>(model(Panel)); View = qobject_cast<CGridView*>(Panel->GetFocusView(0));
        QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading() && names(Model).contains("beta.txt"), 10000);
        QCOMPARE(View->CaptureSelection(), QStringList{Key}); QCOMPARE(View->CaptureFocus(), Key);
        QCOMPARE(View->ColumnWidths()->Policies()[View->ColumnWidths()->ColumnId(0)].value_, 35.0);
        QCOMPARE(static_cast<Qtitan::GridTableColumn*>(View->BaseView()->getColumnByModelColumn(0))->sortOrder(), Qtitan::SortDescending);
        QVERIFY(main_->SaveWorkspace());
        QProcess Restart; auto Environment = QProcessEnvironment::systemEnvironment();
        Environment.insert("HC_WORKSPACE_BOOT_ID", Tab.id_); Restart.setProcessEnvironment(Environment);
        Environment.insert("HC_TEST_CONFIG_DIR", QFileInfo(hc::settings::AppSettings().FileName()).absolutePath());
        Restart.setProcessEnvironment(Environment);
        Restart.start(QCoreApplication::applicationFilePath(), {"workspaceBootUi", "-o", "workspace-restart.txt,txt"});
        QVERIFY(Restart.waitForStarted(5000));
        QTRY_VERIFY_WITH_TIMEOUT(Restart.state() == QProcess::NotRunning, 20000);
        QCOMPARE(Restart.exitStatus(), QProcess::NormalExit); QCOMPARE(Restart.exitCode(), 0);
        auto* Tabs = Panel->findChild<QTabWidget*>("tabWidget"); Tabs->setCurrentIndex(1);
        Model = static_cast<CFSModel*>(model(Panel)); QVERIFY(Model->HasPlaceholder()); QCOMPARE(Model->rowCount(), 0);
        QCOMPARE(Model->WorkspaceLocation(), Saved.tabs_[1].location_);
        QTest::mouseClick(Panel->findChild<QPushButton*>("hc.workspaceRetry"), Qt::LeftButton);
        QVERIFY(Model->HasPlaceholder()); QCOMPARE(Panel->CaptureWorkspace().tabs_[1].provider_, Missing.provider_);
        Tabs->setCurrentIndex(0); Panel->CloseTab(); // Closing active tab updates watches and placeholder status.
        QVERIFY(static_cast<CFSModel*>(model(Panel))->HasPlaceholder());
    }

    void protectedParentNavigationUi()
    {
        auto* AdminIndicator = main_->findChild<QLabel*>("hc.adminIndicator"); QVERIFY(AdminIndicator);
        QCOMPARE(AdminIndicator->isVisible(), hc::permissions::Elevated());
        // The product requests fresh child metadata, unlike a plain provider
        // listing. Exercise its real navigation path with a protected sibling.
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        const auto Start = Directory.filePath("start"); QVERIFY(QDir().mkdir(Start));
        const auto Protected = Directory.filePath("protected"); QVERIFY(QDir().mkdir(Protected));
        const auto Handle = CreateFileW(QDir::toNativeSeparators(Protected).toStdWString().c_str(), READ_CONTROL | WRITE_DAC,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        QVERIFY(Handle != INVALID_HANDLE_VALUE);
        const auto Close = qScopeGuard([&] { CloseHandle(Handle); });
        PACL OriginalAcl = nullptr; PSECURITY_DESCRIPTOR OriginalDescriptor = nullptr;
        QCOMPARE(GetSecurityInfo(Handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
            nullptr, nullptr, &OriginalAcl, nullptr, &OriginalDescriptor), DWORD(ERROR_SUCCESS));
        const auto Free = qScopeGuard([&] { LocalFree(OriginalDescriptor); });
        const auto Restore = qScopeGuard([&] {
            SetSecurityInfo(Handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
                nullptr, nullptr, OriginalAcl, nullptr);
        });
        ACL EmptyAcl{}; QVERIFY(InitializeAcl(&EmptyAcl, sizeof(EmptyAcl), ACL_REVISION));
        QCOMPARE(SetSecurityInfo(Handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, &EmptyAcl, nullptr), DWORD(ERROR_SUCCESS));
        auto* Panel = panels_.front();
        hc::workspace::TabState Tab; Tab.id_ = "protected-parent";
        Tab.location_ = {hc::storage::LocatorKind::Local, Start.toStdString()};
        hc::workspace::PaneState Pane; Pane.tabs_ = {Tab}; Panel->RestoreWorkspace(Pane);
        auto* Model = static_cast<CFSModel*>(model(Panel));
        QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 10000); QVERIFY(!Model->LastNavigationError());
        Model->ChangeDirectory("..");
        QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 10000); QVERIFY(!Model->LastNavigationError());
        QCOMPARE(QDir::cleanPath(QDir::fromNativeSeparators(Model->GetCurrentPathWithRoot())), Directory.path());
        QVERIFY(names(Model).contains("protected")); QVERIFY(names(Model).contains("start"));
        for (int Row = 0; Row < Model->rowCount(); ++Row) {
            const auto Item = Model->GetFileInfo(Model->index(Row, 0));
            if (Item->Name_ == "protected") {
                QVERIFY(Item->storageEntry_); QVERIFY(Item->storageEntry_->directory_);
                QVERIFY(!Item->storageEntry_->id_.revision_);
            }
        }
        Model->Refresh(); QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 10000);
        QVERIFY(!Model->LastNavigationError()); QVERIFY(names(Model).contains("protected"));
        Model->ChangeDirectory("protected"); QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 10000);
        QVERIFY(Model->LastNavigationError());
        QCOMPARE(Model->LastNavigationError()->category_, hc::storage::ErrorCategory::AccessDenied);
        QCOMPARE(QDir::cleanPath(QDir::fromNativeSeparators(Model->GetCurrentPathWithRoot())), Directory.path());
        QTRY_VERIFY(Panel->findChild<QMessageBox*>("hc.navigationError"));
        QPointer<QMessageBox> Failure = Panel->findChild<QMessageBox*>("hc.navigationError");
        if (!hc::permissions::Elevated()) {
            auto* Elevate = Failure->findChild<QPushButton*>("hc.navigationElevate"); QVERIFY(Elevate);
            QCOMPARE(Elevate->text(), QStringLiteral("관리자 권한으로 재시도"));
        }
        QVERIFY(Failure->text().contains(QDir::toNativeSeparators(Protected)));
        QVERIFY(Failure->detailedText().contains("win32:5"));
        QVERIFY(!Failure->informativeText().contains("win32:"));
        const auto Artifacts = qEnvironmentVariable("HC_UI_ARTIFACT_DIR");
        if (!Artifacts.isEmpty()) {
            QDir().mkpath(Artifacts);
            QVERIFY(Failure->grab().save(QDir(Artifacts).filePath("navigation-error.png")));
            QVERIFY(AdminIndicator->parentWidget()->grab().save(QDir(Artifacts).filePath("admin-header.png")));
        }
        const auto FailedGeneration = Model->DirectoryGeneration();
        QTest::mouseClick(Failure->button(QMessageBox::Retry), Qt::LeftButton);
        QTRY_VERIFY(Failure.isNull());
        QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 10000);
        QVERIFY(Model->DirectoryGeneration() > FailedGeneration);
        QVERIFY(Model->LastNavigationError()); // Retry must target protected, not its readable parent.
        QTRY_VERIFY(Panel->findChild<QMessageBox*>("hc.navigationError"));
        Failure = Panel->findChild<QMessageBox*>("hc.navigationError");
        QTest::keyClick(Failure, Qt::Key_Escape);
        QTRY_VERIFY(Failure.isNull());
        QVERIFY(!Model->LastNavigationError());
        QTRY_VERIFY(Panel->GetFocusView(Panel->CurrentTabIndex())->isAncestorOf(QApplication::focusWidget())
            || Panel->GetFocusView(Panel->CurrentTabIndex()) == QApplication::focusWidget());
        QVERIFY(names(Model).contains("protected"));
        Model->ChangeDirectory("protected"); QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 10000);
        QTRY_VERIFY(Panel->findChild<QMessageBox*>("hc.navigationError"));
        Failure = Panel->findChild<QMessageBox*>("hc.navigationError");
        QCOMPARE(SetSecurityInfo(Handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, OriginalAcl, nullptr), DWORD(ERROR_SUCCESS));
        QTest::mouseClick(Failure->button(QMessageBox::Retry), Qt::LeftButton);
        QTRY_VERIFY(Failure.isNull());
        QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 10000);
        QVERIFY(!Model->LastNavigationError());
        QCOMPARE(QDir::cleanPath(QDir::fromNativeSeparators(Model->GetCurrentPathWithRoot())), Protected);
        Model->ChangeDirectory(".."); QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 10000);
        Model->ChangeDirectory("start"); QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 10000);
        QVERIFY(!Model->LastNavigationError());

        // Read-only reproduction on the actual Windows volume; never change
        // system ACLs. Record the token used to distinguish UAC elevation.
        HANDLE Token = nullptr; QVERIFY(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token));
        const auto CloseToken = qScopeGuard([&] { CloseHandle(Token); });
        TOKEN_ELEVATION Elevation{}; DWORD Bytes = 0;
        QVERIFY(GetTokenInformation(Token, TokenElevation, &Elevation, sizeof(Elevation), &Bytes));
        qInfo() << "Parent navigation token elevated:" << Elevation.TokenIsElevated;
        const auto Users = QDir::cleanPath(QDir::homePath() + "/..");
        Tab.location_.root_ = Users.toStdString(); Pane.tabs_ = {Tab}; Panel->RestoreWorkspace(Pane);
        Model = static_cast<CFSModel*>(model(Panel));
        QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 10000); QVERIFY(!Model->LastNavigationError());
        Model->ChangeDirectory(".."); QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 10000);
        QVERIFY(!Model->LastNavigationError());
        QCOMPARE(QDir::cleanPath(QDir::fromNativeSeparators(Model->GetCurrentPathWithRoot())), QDir::cleanPath(Users + "/.."));
        QVERIFY(names(Model).contains(QFileInfo(Users).fileName()));
    }

    void workspaceWatchAndMetadataUi()
    {
        using namespace hc::workspace;
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        const auto Path = Directory.filePath("watched.txt");
        { QFile File(Path); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("one"); }
        TabState Tab; Tab.id_ = "m7-watch"; Tab.location_ = {hc::storage::LocatorKind::Local, Directory.path().toStdString()};
        PaneState Pane; Pane.tabs_ = {Tab}; auto* Panel = panels_.front(); Panel->RestoreWorkspace(Pane);
        auto* Model = static_cast<CFSModel*>(model(Panel)); auto* View = qobject_cast<CGridView*>(Panel->GetFocusView(0));
        QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading() && names(Model).contains("watched.txt"), 10000);
        hc::storage::EntrySnapshot Entry;
        for (int Row = 0; Row < Model->rowCount(); ++Row) {
            const auto Item = Model->GetFileInfo(Model->index(Row, 0)); if (Item && Item->storageEntry_) Entry = *Item->storageEntry_;
        }
        const auto Key = SelectionKey(Entry); View->RestoreSelection({Key}, Key);
        const auto Generation = Model->MetadataGeneration();
        QVERIFY(Model->PutMetadata(Entry, {{"author", "Kim"}}, Generation));
        QVERIFY(Model->MatchesSearch(Entry, "author:kim ext:txt"));
        auto* Search = Panel->findChild<QLineEdit*>("hc.workspaceSearch"); Search->setText("author:kim");
        QCOMPARE(View->BaseView()->getRowCount(), 2); // Parent plus matching entry.
        Search->clear(); View->RestoreSelection({Key}, Key);
        { QFile File(Directory.filePath("created.txt")); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("new"); }
        QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading() && names(Model).contains("created.txt"), 10000);
        QCOMPARE(View->CaptureSelection(), QStringList{Key}); QCOMPARE(View->CaptureFocus(), Key);
        QVERIFY(Model->MatchesSearch(Entry, "author:kim"));
        { QFile File(Path); QVERIFY(File.open(QIODevice::Append)); File.write("changed"); }
        Model->Refresh(); QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 10000);
        QVERIFY(!Model->MatchesSearch(Entry, "author:kim"));
        QVERIFY(!Model->PutMetadata(Entry, {{"author", "stale"}}, Generation));
        QVERIFY(QFile::rename(Path, Directory.filePath("renamed.txt")));
        QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading() && names(Model).contains("renamed.txt"), 10000);
        // Native enumeration deliberately uses session/path identity to avoid a
        // per-entry handle open. External renames must not select another row.
        QCOMPARE(Entry.id_.scope_, hc::storage::IdentityScope::NavigationSession);
        QVERIFY(View->CaptureSelection().isEmpty());
        Search->setText("ext:txt name:renamed");
        const auto Saved = Panel->CaptureWorkspace(); QCOMPARE(Saved.tabs_[0].view_.value("search").toString(), Search->text());
        qInfo() << "M7 workspace restore, watch resync, stable selection and metadata invalidation passed";
    }

    void scopedSearchUi()
    {
        QTemporaryDir Directory; QVERIFY(Directory.isValid()); QDir(Directory.path()).mkdir("nested");
        { QFile File(Directory.filePath("nested/found.txt")); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("result"); }
        auto* Panel = panels_.front(); hc::workspace::TabState Tab; Tab.id_ = "m7-search";
        Tab.location_ = {hc::storage::LocatorKind::Local, Directory.path().toStdString()};
        hc::workspace::PaneState Pane; Pane.tabs_ = {Tab}; Panel->RestoreWorkspace(Pane);
        auto* Model = static_cast<CFSModel*>(model(Panel)); QTRY_VERIFY(!Model->IsLoading() && names(Model).contains("nested"));
        Panel->sig_NotifyPanelActivated();
        auto* Action = main_->findChild<QAction*>("hc.menu.search"); QVERIFY(Action); Action->trigger();
        auto* Dialog = main_->findChild<QDialog*>("hc.searchDialog"); QVERIFY(Dialog);
        Dialog->findChild<QLineEdit*>("hc.search.query")->setText("ext:txt");
        Dialog->findChild<QPushButton*>("hc.search.start")->click();
        auto* Results = Dialog->findChild<QTreeWidget*>("hc.search.results"); QTRY_COMPARE(Results->topLevelItemCount(), 1);
        QMetaObject::invokeMethod(Results, "itemDoubleClicked", Q_ARG(QTreeWidgetItem*, Results->topLevelItem(0)), Q_ARG(int, 0));
        QTRY_VERIFY(!Model->IsLoading() && names(Model).contains("found.txt"));
        QPointer<QDialog> Guard(Dialog); Dialog->close(); QTRY_VERIFY(Guard.isNull());
    }

    void automationCommandsUi()
    {
        using namespace hc::automation;
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        auto* Panel = panels_.front(); hc::workspace::TabState Tab; Tab.id_ = "m8-automation";
        Tab.location_ = {hc::storage::LocatorKind::Local, Directory.path().toStdString()};
        hc::workspace::PaneState Pane; Pane.tabs_ = {Tab}; Panel->RestoreWorkspace(Pane); Panel->sig_NotifyPanelActivated();
        auto* Model = static_cast<CFSModel*>(model(Panel)); QTRY_VERIFY(!Model->IsLoading() && Model->GetRoot());
        auto* Bridge = TyStCommandMgr::GetInstance()->Bridge(); QVERIFY(Bridge);
        const auto Statement = [](QString Id, QJsonObject Parameters) { return "invoke \"" + Id + "\" " + QString::fromUtf8(QJsonDocument(Parameters).toJson(QJsonDocument::Compact)) + '\n'; };
        auto* Action = main_->findChild<QAction*>("hc.menu.automation"); QVERIFY(Action); Action->trigger();
        auto* Dialog = main_->findChild<QDialog*>("hc.automationDialog"); QVERIFY(Dialog);
        const auto Created = Directory.filePath("script-created");
        auto* Source = Dialog->findChild<QPlainTextEdit*>("hc.automation.source");
        auto* Run = Dialog->findChild<QPushButton*>("hc.automation.run"); auto* Status = Dialog->findChild<QLabel*>("hc.automation.status");
        Source->setPlainText(Statement("hc.file.mkdir", {{"destination", Created}})); Run->click();
        QTRY_VERIFY(Run->isEnabled()); QVERIFY(Status->text().contains("permission-required")); QVERIFY(!QFileInfo::exists(Created));
        Dialog->findChild<QCheckBox*>("hc.automation.write")->setChecked(true);
        Source->setPlainText(Statement("hc.file.mkdir", {{"destination", Created}})
            + Statement("hc.navigate.path", {{"path", Created}}) + Statement("hc.navigate.refresh", {}));
        Run->click(); QTRY_VERIFY_WITH_TIMEOUT(Run->isEnabled(), 10000);
        QVERIFY2(Status->text().startsWith(QString::fromUtf8("완료")), qPrintable(Status->text()));
        QVERIFY(QFileInfo(Created).isDir()); QCOMPARE(QDir::cleanPath(QDir::fromNativeSeparators(Model->GetCurrentPathWithRoot())), Created);
        // A direct UI invocation and DSL invocation use the same file job path.
        const auto DirectPath = Directory.filePath("direct-created");
        auto Direct = Bridge->Dispatch("hc.file.mkdir", {{"destination", DirectPath}}, "test");
        QTRY_VERIFY(Direct.wait_for(std::chrono::seconds(0)) == std::future_status::ready); auto Accepted = Direct.get(); QVERIFY(Accepted.HasValue());
        QTRY_VERIFY(QFileInfo(DirectPath).isDir());
        const auto Job = Accepted.Value().jobId_;
        QTRY_VERIFY(!Bridge->FileJobs()->IsUiPending(Job) && !Model->IsLoading());
        using ScriptValue = hc::script::ScriptValue;
        using ScriptObject = ScriptValue::Object;
        auto Api = Bridge->ScriptApi({"file-write", "navigation"});
        auto Snapshot = Api->Call("capture", ScriptObject{}); QVERIFY(Snapshot.HasValue());
        const auto ContextHandle = std::get<ScriptObject>(Snapshot.Value().data_).at("context");
        const auto ApiPath = Directory.filePath("api-created");
        auto ApiJob = Api->Call("invoke", ScriptObject{{"context", ContextHandle}, {"command", "hc.file.mkdir"},
            {"parameters", ScriptObject{{"destination", ApiPath.toStdString()}}}}); QVERIFY(ApiJob.HasValue());
        const auto ApiDone = [&] {
            auto Result = Api->Call("poll", ScriptObject{{"handle", ApiJob.Value()}});
            return Result.HasValue() && std::get<bool>(std::get<ScriptObject>(Result.Value().data_).at("done").data_);
        };
        QTRY_VERIFY_WITH_TIMEOUT(ApiDone(), 10000); QVERIFY(QFileInfo(ApiPath).isDir());
        const auto CancelPath = Directory.filePath("api-cancelled");
        auto CancelJob = Api->Call("invoke", ScriptObject{{"context", ContextHandle}, {"command", "hc.file.mkdir"},
            {"parameters", ScriptObject{{"destination", CancelPath.toStdString()}}}}); QVERIFY(CancelJob.HasValue());
        QVERIFY(Api->Call("cancel", ScriptObject{{"handle", CancelJob.Value()}}).HasValue());
        const auto RetryPath = Directory.filePath("api-after-cancel");
        ApiJob = Api->Call("invoke", ScriptObject{{"context", ContextHandle}, {"command", "hc.file.mkdir"},
            {"parameters", ScriptObject{{"destination", RetryPath.toStdString()}}}}); QVERIFY(ApiJob.HasValue());
        QTRY_VERIFY_WITH_TIMEOUT(ApiDone(), 10000); QVERIFY(QFileInfo(RetryPath).isDir()); QVERIFY(!QFileInfo::exists(CancelPath));
        // Execute the selected runtime through the real editor, command bus,
        // scheduler and product event hub, not a benchmark host.
        Dialog->findChild<QComboBox*>("hc.automation.language")->setCurrentIndex(1);
        const QString LuaSource = R"lua(
local workspace = hc.workspace.snapshot()
assert(#workspace.panes >= 2)
local subscription = hc.events.subscribe('job.completed')
local c = hc.selection.snapshot()
local job = hc.fs.mkdir(c.context, {destination=c.path .. '/lua-created'})
local first = hc.job.snapshot(job)
assert(first.state ~= nil)
hc.job.await(job)
local events = hc.events.poll(subscription)
assert(events.dropped == 0 and #events.events >= 1)
hc.events.unsubscribe(subscription)
hc.release(job)
)lua";
        Source->setPlainText(LuaSource); Run->click(); QTRY_VERIFY_WITH_TIMEOUT(Run->isEnabled(),10000);
        QVERIFY(Status->text().contains("permission-required")); QVERIFY(!QFileInfo::exists(Created + "/lua-created"));
        Dialog->findChild<QCheckBox*>("hc.automation.write")->setChecked(true); Run->click();
        QTRY_VERIFY_WITH_TIMEOUT(Run->isEnabled(),10000); QVERIFY2(Status->text().contains("Lua 실행 완료"),qPrintable(Status->text()));
        QVERIFY(QFileInfo(Created + "/lua-created").isDir());
        QFile TextFile(Created + "/lua-viewer.txt"); QVERIFY(TextFile.open(QIODevice::WriteOnly)); TextFile.write("Lua viewer integration\n"); TextFile.close();
        auto Refresh = Bridge->Dispatch("hc.navigate.refresh",{},"test");
        QTRY_VERIFY(Refresh.wait_for(std::chrono::seconds(0)) == std::future_status::ready); QVERIFY(Refresh.get().HasValue());
        QTRY_VERIFY(!Model->IsLoading());
        auto* Grid = static_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex())); QVERIFY(Grid);
        bool Selected = false;
        for (int Row=0;Row<Grid->BaseView()->modelController()->getGridRowCount();++Row) {
            Grid->BaseView()->setFocusedRowIndex(Row); auto Index = Grid->BaseView()->focusedRow().modelIndex(0);
            while (const auto* Proxy = qobject_cast<const QAbstractProxyModel*>(Index.model())) Index = Proxy->mapToSource(Index);
            const auto Entry = Model->GetFileInfo(Index);
            if (Entry && Entry->Name_ == "lua-viewer.txt") { Selected = true; break; }
        }
        QVERIFY(Selected);
        Source->setPlainText(R"lua(
local c = hc.capture()
assert(#c.selection == 1)
assert(hc.fs.resource(c.selection[1]).provider ~= nil)
local sub = hc.events.subscribe('viewer.ready')
local viewer = hc.viewer.open(c.selection[1])
while hc.viewer.snapshot(viewer).state == 'loading' do hc.sleep(10) end
assert(hc.viewer.snapshot(viewer).state == 'ready')
hc.viewer.execute(viewer, 'zoom-in')
local batch = hc.events.poll(sub)
assert(#batch.events >= 1)
hc.viewer.close(viewer)
assert(not hc.call('viewer.snapshot',{handle=viewer}).ok)
hc.events.unsubscribe(sub)
)lua");
        Run->click(); QTRY_VERIFY_WITH_TIMEOUT(Run->isEnabled(),10000);
        QVERIFY2(Status->text().contains("Lua 실행 완료"),qPrintable(Status->text()));
        QTRY_VERIFY(main_->findChildren<QDialog*>("hc.viewer.full").isEmpty());
        Execution Closed(Compile("invoke \"hc.navigate.refresh\" {}").Value(), Bridge->AutomationHost(), {"navigation"}, "closed-tab");
        Execution Queued(Compile("invoke \"hc.file.mkdir\" {\"destination\":\"" + Directory.filePath("must-not-create") + "\"}").Value(), Bridge->AutomationHost(), {"file-write"}, "queued-tab");
        Queued.Tick(); // Recreate the same persistent tab ID before dispatch runs.
        Pane.tabs_ = {Tab}; Panel->RestoreWorkspace(Pane);
        QVERIFY(!Api->Call("invoke", ScriptObject{{"context", ContextHandle}, {"command", "hc.navigate.refresh"}, {"parameters", ScriptObject{}}}).HasValue());
        Api->Close();
        Closed.Tick(); QCOMPARE(Closed.Status(), State::Cancelled); QCOMPARE(Closed.Failure()->code_, std::string("automation-tab-closed"));
        QTRY_VERIFY((Queued.Tick(), Queued.Status() != State::Running)); QCOMPARE(Queued.Status(), State::Cancelled);
        QVERIFY(!QFileInfo::exists(Directory.filePath("must-not-create")));
        QPointer<QDialog> Guard(Dialog); Dialog->close(); QTRY_VERIFY(Guard.isNull());
        Model = static_cast<CFSModel*>(model(Panel)); QTRY_VERIFY(!Model->IsLoading());
        auto Provider = std::make_shared<GatedProvider>(); auto Release = qScopeGuard([&] { Provider->release_.set_value(); });
        Model->SetRoot(nsHC::TySpFileSystem(new GatedFs(Provider))); Model->SetCurrentPath("\\");
        Execution Cancelled(Compile("invoke \"hc.navigate.refresh\" {}").Value(), Bridge->AutomationHost(), {"navigation"}, "cancel-navigation");
        Cancelled.Tick(); QTRY_VERIFY(Provider->started_.load()); QVERIFY(Model->IsLoading());
        // Cancel before the execution consumes the Accepted future as well.
        Cancelled.Cancel(); QCOMPARE(Cancelled.Status(), State::Cancelled); QVERIFY(!Model->IsLoading());
        QVERIFY(Model->property("hc.correlation").toString().isEmpty());
        Execution Replaced(Compile("invoke \"hc.navigate.refresh\" {}").Value(), Bridge->AutomationHost(), {"navigation"}, "replaced-navigation");
        Replaced.Tick(); QTRY_VERIFY(Model->IsLoading());
        auto Manual = Bridge->Dispatch("hc.navigate.refresh", {}, "test");
        QTRY_VERIFY(Manual.wait_for(std::chrono::seconds(0)) == std::future_status::ready); QVERIFY(Manual.get().HasValue());
        Replaced.Tick(); QCOMPARE(Replaced.Status(), State::Cancelled);
        QVERIFY(Model->IsLoading()); // Script cleanup must not cancel the user's replacement request.
        Model->CancelNavigation();
    }

    void workspaceSmbRestore()
    {
        if (localOnly_) QSKIP("Requires the authorized live SMB share");
        using namespace hc::workspace;
        TabState Tab; Tab.id_ = "m7-smb"; Tab.location_ = {hc::storage::LocatorKind::Smb, unc_.toStdString()};
        PaneState Pane; Pane.tabs_ = {Tab}; auto* Panel = panels_.front();
        QElapsedTimer Timer; Timer.start(); Panel->RestoreWorkspace(Pane);
        qInfo() << "SMB workspace restore UI call (ms):" << Timer.elapsed(); QVERIFY(Timer.elapsed() < 500);
        auto* Model = static_cast<CFSModel*>(model(Panel));
        QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 15000);
        QVERIFY(!Model->LastNavigationError()); QVERIFY(Model->GetRoot()->IsRemote());
        QCOMPARE(Model->WorkspaceLocation().kind_, hc::storage::LocatorKind::Smb);
        using namespace hc::columns;
        auto Profile = DefaultProfile(); ScriptDefinition Script; Script.title_ = "SMB snapshot";
        Script.dependencies_ = {"file.name"}; Script.outputs_ = {{"label", "Label", FieldType::Text}};
        Script.source_ = "return function(item) return {label=item['file.name']..':SMB'} end";
        Profile.scripts_.append(Script); Profile.columns_[1].fieldId_ = ScriptPrefix(Script) + "label";
        QVERIFY(static_cast<CGridView*>(Panel->GetFocusView(0))->ApplyColumnProfile(Profile));
        const auto CheckColumn = [&] {
            for (int Row = 0; Row < Model->rowCount(); ++Row) if (Model->GetName(Model->index(Row, 0)) != "..") {
                const auto Cell = Model->index(Row, 1); Cell.data();
                QTRY_COMPARE_WITH_TIMEOUT(Cell.data().toString(), Model->GetName(Model->index(Row, 0)) + ":SMB", 10000); return;
            }
            QFAIL("The authorized SMB share must contain a sample entry");
        };
        CheckColumn();
        const auto Expected = names(Model); const auto Saved = Panel->CaptureWorkspace();
        Panel->RestoreWorkspace(Saved); Model = static_cast<CFSModel*>(model(Panel));
        QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 15000); QVERIFY(!Model->LastNavigationError()); QCOMPARE(names(Model), Expected);
        CheckColumn();
        auto Missing = Saved; Missing.tabs_[0].location_.root_ += "\\__hc_m7_missing_" + QUuid::createUuid().toString(QUuid::Id128).toStdString();
        Panel->RestoreWorkspace(Missing); Model = static_cast<CFSModel*>(model(Panel));
        QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 15000); QVERIFY(Model->LastNavigationError()); QCOMPARE(Model->rowCount(), 0);
        QCOMPARE(Model->WorkspaceLocation(), Missing.tabs_[0].location_);
        Panel->RestoreWorkspace(Saved); Model = static_cast<CFSModel*>(model(Panel));
        QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 15000); QVERIFY(!Model->LastNavigationError()); QCOMPARE(names(Model), Expected);
    }

    void workspaceBootUi()
    {
        const auto Expected = qEnvironmentVariable("HC_WORKSPACE_BOOT_ID");
        if (Expected.isEmpty()) QSKIP("Invoked by workspace restart test");
        auto* Panel = panels_.front(); const auto Saved = Panel->CaptureWorkspace();
        QCOMPARE(Saved.tabs_.size(), 2); QCOMPARE(Saved.tabs_[0].id_, Expected);
        QCOMPARE(Saved.active_, 0); QCOMPARE(Saved.tabs_[0].selection_.size(), 1);
        auto* View = qobject_cast<CGridView*>(Panel->GetFocusView(0));
        QCOMPARE(View->CaptureSelection(), Saved.tabs_[0].selection_);
        QCOMPARE(View->ColumnWidths()->Policies()[View->ColumnWidths()->ColumnId(0)].value_, 35.0);
        QVERIFY(!qobject_cast<CGridView*>(Panel->GetFocusView(1))->BaseModel()->GetRoot());
    }

    void isoPluginUi_data()
    {
        QTest::addColumn<int>("ProfileIndex"); QTest::newRow("joliet-udf") << 2; QTest::newRow("long-name-udf") << 3;
    }
    void r19ArchiveBenchmark_data() {
        QTest::addColumn<bool>("Materialized");
        QTest::newRow("materialized") << true; QTest::newRow("streaming") << false;
    }
    void r19ArchiveBenchmark() {
        QFETCH(bool,Materialized); using namespace hc::operations; using namespace hc::storage;
        const auto Source=qEnvironmentVariable("HC_R18_ARCHIVE_FILE"); if(Source.isEmpty()) QSKIP("Set the large archive source explicitly");
        QTemporaryDir Directory(QDir::current().filePath("r19-benchmark-XXXXXX")); QVERIFY(Directory.isValid());
        const auto Link=QDir::toNativeSeparators(Directory.filePath(QFileInfo(Source).fileName())), Original=QDir::toNativeSeparators(Source);
        QVERIFY(CreateHardLinkW(reinterpret_cast<LPCWSTR>(Link.utf16()),reinterpret_cast<LPCWSTR>(Original.utf16()),nullptr));
        auto Native=MakeNativeReadProvider(LocatorKind::Local); auto Writer=MakeNativeWritableStorage(LocatorKind::Local,Native);
        auto Entry=Native->Resolve({LocatorKind::Local,Link.toStdString()},{}).get(); QVERIFY(Entry.HasValue());
        auto Backend=hc::plugins::MakeApprovedWcxBackend(hc::plugins::BuiltinSevenZipOptions());
        ExtractRequest Request; Request.archives_={{Entry.Value(),Native,{}}}; Request.sourceRoot_={LocatorKind::Local,Directory.path().toStdString()};
        Request.destination_={LocatorKind::Local,Directory.path().toStdString()}; Request.writer_=Writer;
        Request.readers_=[Backend,Materialized,Base=Directory.path().toStdString()](const EntrySnapshot& Entry) {
            Locator Root{LocatorKind::Archive,Entry.locator_.root_,{{"wcx","/"}}};
            return std::vector<std::shared_ptr<IStorageProvider>>{MakeWcxReadProvider({},Root,Base,Backend,{},true,
                Materialized?ArchiveReadPolicy::Materialized:ArchiveReadPolicy::Streaming)};
        };
        auto Prepared=PrepareExtract(std::move(Request)); QVERIFY(Prepared.HasValue());
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(),1,1024,MakeFileJobJournal(Directory.filePath("journal"),true,false));
        OperationMetrics::Instance().Reset(); QElapsedTimer Clock; Clock.start();
        auto Handle=Scheduler.Submit(std::move(Prepared.Value().request_),std::move(Prepared.Value().task_)); QVERIFY(Handle.HasValue());
        int NextLog=0;
        while(Handle.Value().completion_.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready && Clock.elapsed()<900000) {
            if(Clock.elapsed()>=NextLog) { auto Job=Scheduler.Snapshot(Handle.Value().id_); qInfo()<<"R19"<<(Materialized?"materialized":"streaming")<<Clock.elapsed()<<"bytes"<<qulonglong(Job.Value().progress_.bytesDone_)<<"items"<<qulonglong(Job.Value().completedItems_); NextLog+=10000; }
            QTest::qWait(50);
        }
        if(Handle.Value().completion_.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready) Scheduler.Cancel(Handle.Value().id_);
        QTRY_VERIFY_WITH_TIMEOUT(Handle.Value().completion_.wait_for(std::chrono::milliseconds(0))==std::future_status::ready,30000);
        const auto Final=Handle.Value().completion_.get(); const auto Wall=Clock.elapsed();
        QVERIFY(OperationMetrics::Instance().Save(QDir::current().filePath(Materialized?"r19-materialized-metrics.json":"r19-streaming-metrics.json"),Wall));
        QCOMPARE(Final.snapshot_.state_,JobState::Completed);
        QDirIterator Files(Directory.filePath(QFileInfo(Source).completeBaseName()),QDir::Files|QDir::Hidden|QDir::System,QDirIterator::Subdirectories);
        std::uint64_t Count=0,Bytes=0; while(Files.hasNext()){Files.next();++Count;Bytes+=Files.fileInfo().size();}
        QCOMPARE(Bytes,Final.snapshot_.progress_.bytesDone_); QCOMPARE(Count,std::uint64_t(23362)); QCOMPARE(Bytes,std::uint64_t(20394591999));
        qInfo()<<"R19 verified"<<(Materialized?"materialized":"streaming")<<Wall<<qulonglong(Count)<<qulonglong(Bytes);
    }
    void r20ArchiveBenchmark_data() {
        QTest::addColumn<bool>("SaveProgress");
        QTest::newRow("progress-on") << true; QTest::newRow("progress-off") << false;
    }
    void r20ArchiveBenchmark() {
        QFETCH(bool,SaveProgress); using namespace hc::operations; using namespace hc::storage;
        const auto Source=qEnvironmentVariable("HC_R18_ARCHIVE_FILE"); if(Source.isEmpty()) QSKIP("Set the large archive source explicitly");
        QTemporaryDir Directory(QDir::current().filePath("r20-benchmark-XXXXXX")); QVERIFY(Directory.isValid());
        const auto Link=QDir::toNativeSeparators(Directory.filePath(QFileInfo(Source).fileName())), Original=QDir::toNativeSeparators(Source);
        QVERIFY(CreateHardLinkW(reinterpret_cast<LPCWSTR>(Link.utf16()),reinterpret_cast<LPCWSTR>(Original.utf16()),nullptr));
        auto Native=MakeNativeReadProvider(LocatorKind::Local); auto Writer=MakeNativeWritableStorage(LocatorKind::Local,Native);
        auto Entry=Native->Resolve({LocatorKind::Local,Link.toStdString()},{}).get(); QVERIFY(Entry.HasValue());
        auto Backend=hc::plugins::MakeApprovedWcxBackend(hc::plugins::BuiltinSevenZipOptions());
        ExtractRequest Request; Request.archives_={{Entry.Value(),Native,{}}}; Request.sourceRoot_={LocatorKind::Local,Directory.path().toStdString()};
        Request.destination_={LocatorKind::Local,Directory.path().toStdString()}; Request.writer_=Writer;
        Request.readers_=[Backend,SaveProgress,Base=Directory.path().toStdString()](const EntrySnapshot& Entry) {
            Locator Root{LocatorKind::Archive,Entry.locator_.root_,{{"wcx","/"}}};
            return std::vector<std::shared_ptr<IStorageProvider>>{MakeWcxReadProvider({},Root,Base,Backend,{},true,
                ArchiveReadPolicy::Streaming)};
        };
        auto Prepared=PrepareExtract(std::move(Request)); QVERIFY(Prepared.HasValue());
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(),1,1024,MakeFileJobJournal(Directory.filePath("journal"),SaveProgress,false));
        OperationMetrics::Instance().Reset(); QElapsedTimer Clock; Clock.start();
        auto Handle=Scheduler.Submit(std::move(Prepared.Value().request_),std::move(Prepared.Value().task_)); QVERIFY(Handle.HasValue());
        int NextLog=0;
        while(Handle.Value().completion_.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready && Clock.elapsed()<900000) {
            if(Clock.elapsed()>=NextLog) { auto Job=Scheduler.Snapshot(Handle.Value().id_); qInfo()<<"R20"<<(SaveProgress?"progress-on":"progress-off")<<Clock.elapsed()<<"bytes"<<qulonglong(Job.Value().progress_.bytesDone_)<<"items"<<qulonglong(Job.Value().completedItems_); NextLog+=10000; }
            QTest::qWait(50);
        }
        if(Handle.Value().completion_.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready) Scheduler.Cancel(Handle.Value().id_);
        QTRY_VERIFY_WITH_TIMEOUT(Handle.Value().completion_.wait_for(std::chrono::milliseconds(0))==std::future_status::ready,30000);
        const auto Final=Handle.Value().completion_.get(); const auto Wall=Clock.elapsed();
        QVERIFY(OperationMetrics::Instance().Save(QDir::current().filePath(SaveProgress?"r20-progress-on-metrics.json":"r20-progress-off-metrics.json"),Wall));
        QCOMPARE(Final.snapshot_.state_,JobState::Completed);
        QDirIterator Files(Directory.filePath(QFileInfo(Source).completeBaseName()),QDir::Files|QDir::Hidden|QDir::System,QDirIterator::Subdirectories);
        std::uint64_t Count=0,Bytes=0; while(Files.hasNext()){Files.next();++Count;Bytes+=Files.fileInfo().size();}
        QCOMPARE(Bytes,Final.snapshot_.progress_.bytesDone_); QCOMPARE(Count,std::uint64_t(23362)); QCOMPARE(Bytes,std::uint64_t(20394591999));
        qInfo()<<"R20 verified"<<(SaveProgress?"progress-on":"progress-off")<<Wall<<qulonglong(Count)<<qulonglong(Bytes);
    }
    void r21ArchiveBenchmark_data() {
        QTest::addColumn<bool>("Optimized");
        QTest::newRow("baseline") << false; QTest::newRow("optimized") << true;
    }
    void r21ArchiveBenchmark() {
        QFETCH(bool,Optimized); using namespace hc::operations; using namespace hc::storage;
        const auto Source=qEnvironmentVariable("HC_R18_ARCHIVE_FILE"); if(Source.isEmpty()) QSKIP("Set the large archive source explicitly");
        QTemporaryDir Directory(QDir::current().filePath("r21-benchmark-XXXXXX")); QVERIFY(Directory.isValid());
        const auto Link=QDir::toNativeSeparators(Directory.filePath(QFileInfo(Source).fileName())), Original=QDir::toNativeSeparators(Source);
        QVERIFY(CreateHardLinkW(reinterpret_cast<LPCWSTR>(Link.utf16()),reinterpret_cast<LPCWSTR>(Original.utf16()),nullptr));
        auto Native=MakeNativeReadProvider(LocatorKind::Local); auto Writer=MakeNativeWritableStorage(LocatorKind::Local,Native);
        auto Entry=Native->Resolve({LocatorKind::Local,Link.toStdString()},{}).get(); QVERIFY(Entry.HasValue());
        auto Backend=hc::plugins::MakeApprovedWcxBackend(hc::plugins::BuiltinSevenZipOptions());
        ExtractRequest Request; Request.archives_={{Entry.Value(),Native,{}}}; Request.sourceRoot_={LocatorKind::Local,Directory.path().toStdString()};
        Request.destination_={LocatorKind::Local,Directory.path().toStdString()}; Request.writer_=Writer;
        Request.readers_=[Backend,Optimized,Base=Directory.path().toStdString()](const EntrySnapshot& Entry) {
            Locator Root{LocatorKind::Archive,Entry.locator_.root_,{{"wcx","/"}}};
            return std::vector<std::shared_ptr<IStorageProvider>>{MakeWcxReadProvider({},Root,Base,Backend,{},true,
                ArchiveReadPolicy::Streaming)};
        };
        auto Prepared=PrepareExtract(std::move(Request)); QVERIFY(Prepared.HasValue());
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(),1,1024,MakeFileJobJournal(Directory.filePath("journal"),false,Optimized));
        OperationMetrics::Instance().Reset(); QElapsedTimer Clock; Clock.start();
        auto Handle=Scheduler.Submit(std::move(Prepared.Value().request_),std::move(Prepared.Value().task_)); QVERIFY(Handle.HasValue());
        int NextLog=0;
        while(Handle.Value().completion_.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready && Clock.elapsed()<900000) {
            if(Clock.elapsed()>=NextLog) { auto Job=Scheduler.Snapshot(Handle.Value().id_); qInfo()<<"R21"<<(Optimized?"optimized":"baseline")<<Clock.elapsed()<<"bytes"<<qulonglong(Job.Value().progress_.bytesDone_)<<"items"<<qulonglong(Job.Value().completedItems_); NextLog+=10000; }
            QTest::qWait(50);
        }
        if(Handle.Value().completion_.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready) Scheduler.Cancel(Handle.Value().id_);
        QTRY_VERIFY_WITH_TIMEOUT(Handle.Value().completion_.wait_for(std::chrono::milliseconds(0))==std::future_status::ready,30000);
        const auto Final=Handle.Value().completion_.get(); const auto Wall=Clock.elapsed();
        QVERIFY(OperationMetrics::Instance().Save(QDir::current().filePath(Optimized?"r21-optimized-metrics.json":"r21-baseline-metrics.json"),Wall));
        QCOMPARE(Final.snapshot_.state_,JobState::Completed);
        QDirIterator Files(Directory.filePath(QFileInfo(Source).completeBaseName()),QDir::Files|QDir::Hidden|QDir::System,QDirIterator::Subdirectories);
        std::uint64_t Count=0,Bytes=0; while(Files.hasNext()){Files.next();++Count;Bytes+=Files.fileInfo().size();}
        QCOMPARE(Bytes,Final.snapshot_.progress_.bytesDone_); QCOMPARE(Count,std::uint64_t(23362)); QCOMPARE(Bytes,std::uint64_t(20394591999));
        qInfo()<<"R21 verified"<<(Optimized?"optimized":"baseline")<<Wall<<qulonglong(Count)<<qulonglong(Bytes);
    }
    void r22ArchiveBenchmark_data() {
        QTest::addColumn<bool>("EnableRecovery");
        QTest::newRow("on-1") << true; QTest::newRow("off-1") << false;
        QTest::newRow("off-2") << false; QTest::newRow("on-2") << true;
    }
    void r22ArchiveBenchmark() {
        QFETCH(bool,EnableRecovery); using namespace hc::operations; using namespace hc::storage;
        const auto Source=qEnvironmentVariable("HC_R18_ARCHIVE_FILE"); if(Source.isEmpty()) QSKIP("Set the large archive source explicitly");
        QTemporaryDir Directory(QDir::current().filePath("r22-benchmark-XXXXXX")); QVERIFY(Directory.isValid());
        const auto Link=QDir::toNativeSeparators(Directory.filePath(QFileInfo(Source).fileName())), Original=QDir::toNativeSeparators(Source);
        QVERIFY(CreateHardLinkW(reinterpret_cast<LPCWSTR>(Link.utf16()),reinterpret_cast<LPCWSTR>(Original.utf16()),nullptr));
        auto Native=MakeNativeReadProvider(LocatorKind::Local); auto Writer=MakeNativeWritableStorage(LocatorKind::Local,Native);
        auto Entry=Native->Resolve({LocatorKind::Local,Link.toStdString()},{}).get(); QVERIFY(Entry.HasValue());
        auto Backend=hc::plugins::MakeApprovedWcxBackend(hc::plugins::BuiltinSevenZipOptions());
        ExtractRequest Request; Request.archives_={{Entry.Value(),Native,{}}}; Request.sourceRoot_={LocatorKind::Local,Directory.path().toStdString()};
        Request.destination_={LocatorKind::Local,Directory.path().toStdString()}; Request.writer_=Writer;
        Request.readers_=[Backend,EnableRecovery,Base=Directory.path().toStdString()](const EntrySnapshot& Entry) {
            Locator Root{LocatorKind::Archive,Entry.locator_.root_,{{"wcx","/"}}};
            return std::vector<std::shared_ptr<IStorageProvider>>{MakeWcxReadProvider({},Root,Base,Backend,{},true,
                ArchiveReadPolicy::Streaming)};
        };
        auto Prepared=PrepareExtract(std::move(Request)); QVERIFY(Prepared.HasValue());
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(),1,1024,MakeFileJobJournal(Directory.filePath("journal"),false,true,EnableRecovery));
        OperationMetrics::Instance().Reset(); QElapsedTimer Clock; Clock.start();
        auto Handle=Scheduler.Submit(std::move(Prepared.Value().request_),std::move(Prepared.Value().task_)); QVERIFY(Handle.HasValue());
        int NextLog=0;
        while(Handle.Value().completion_.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready && Clock.elapsed()<900000) {
            if(Clock.elapsed()>=NextLog) { auto Job=Scheduler.Snapshot(Handle.Value().id_); qInfo()<<"R22"<<(EnableRecovery?"recovery-on":"recovery-off")<<Clock.elapsed()<<"bytes"<<qulonglong(Job.Value().progress_.bytesDone_)<<"items"<<qulonglong(Job.Value().completedItems_); NextLog+=10000; }
            QTest::qWait(50);
        }
        if(Handle.Value().completion_.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready) Scheduler.Cancel(Handle.Value().id_);
        QTRY_VERIFY_WITH_TIMEOUT(Handle.Value().completion_.wait_for(std::chrono::milliseconds(0))==std::future_status::ready,30000);
        const auto Final=Handle.Value().completion_.get(); const auto Wall=Clock.elapsed();
        QVERIFY(OperationMetrics::Instance().Save(QDir::current().filePath(QString("r22-%1-metrics.json").arg(QString::fromLatin1(QTest::currentDataTag()))),Wall));
        QCOMPARE(Final.snapshot_.state_,JobState::Completed);
        QCOMPARE(bool(Final.writes_),EnableRecovery);
        QCOMPARE(QDir(Directory.filePath("journal")).exists(),EnableRecovery);
        QDirIterator Files(Directory.filePath(QFileInfo(Source).completeBaseName()),QDir::Files|QDir::Hidden|QDir::System,QDirIterator::Subdirectories);
        std::uint64_t Count=0,Bytes=0; while(Files.hasNext()){Files.next();++Count;Bytes+=Files.fileInfo().size();}
        QCOMPARE(Bytes,Final.snapshot_.progress_.bytesDone_); QCOMPARE(Count,std::uint64_t(23362)); QCOMPARE(Bytes,std::uint64_t(20394591999));
        qInfo()<<"R22 verified"<<(EnableRecovery?"recovery-on":"recovery-off")<<Wall<<qulonglong(Count)<<qulonglong(Bytes);
    }
    void r23ArchiveBenchmark_data() {
        QTest::addColumn<bool>("FlushFileContents");
        QTest::newRow("flush-on-1") << true; QTest::newRow("flush-off-1") << false;
        QTest::newRow("flush-off-2") << false; QTest::newRow("flush-on-2") << true;
    }
    void r23ArchiveBenchmark() {
        QFETCH(bool,FlushFileContents); using namespace hc::operations; using namespace hc::storage;
        const auto Source=qEnvironmentVariable("HC_R18_ARCHIVE_FILE"); if(Source.isEmpty()) QSKIP("Set the large archive source explicitly");
        QTemporaryDir Directory(QDir::current().filePath("r23-benchmark-XXXXXX")); QVERIFY(Directory.isValid());
        const auto Link=QDir::toNativeSeparators(Directory.filePath(QFileInfo(Source).fileName())), Original=QDir::toNativeSeparators(Source);
        QVERIFY(CreateHardLinkW(reinterpret_cast<LPCWSTR>(Link.utf16()),reinterpret_cast<LPCWSTR>(Original.utf16()),nullptr));
        auto Native=MakeNativeReadProvider(LocatorKind::Local); auto Writer=MakeNativeWritableStorage(LocatorKind::Local,Native);
        auto Entry=Native->Resolve({LocatorKind::Local,Link.toStdString()},{}).get(); QVERIFY(Entry.HasValue());
        auto Backend=hc::plugins::MakeApprovedWcxBackend(hc::plugins::BuiltinSevenZipOptions());
        ExtractRequest Request; Request.archives_={{Entry.Value(),Native,{}}}; Request.sourceRoot_={LocatorKind::Local,Directory.path().toStdString()};
        Request.destination_={LocatorKind::Local,Directory.path().toStdString()}; Request.writer_=Writer;
        Request.readers_=[Backend,Base=Directory.path().toStdString()](const EntrySnapshot& Entry) {
            Locator Root{LocatorKind::Archive,Entry.locator_.root_,{{"wcx","/"}}};
            return std::vector<std::shared_ptr<IStorageProvider>>{MakeWcxReadProvider({},Root,Base,Backend,{},true,
                ArchiveReadPolicy::Streaming)};
        };
        OperationContext Context; Context.flushFileContents_=FlushFileContents;
        auto Prepared=PrepareExtract(std::move(Request),Context); QVERIFY(Prepared.HasValue());
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(),1,1024,MakeFileJobJournal(Directory.filePath("journal"),false,true,false));
        OperationMetrics::Instance().Reset(); QElapsedTimer Clock; Clock.start();
        auto Handle=Scheduler.Submit(std::move(Prepared.Value().request_),std::move(Prepared.Value().task_)); QVERIFY(Handle.HasValue());
        int NextLog=0;
        while(Handle.Value().completion_.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready && Clock.elapsed()<900000) {
            if(Clock.elapsed()>=NextLog) { auto Job=Scheduler.Snapshot(Handle.Value().id_); qInfo()<<"R23"<<(FlushFileContents?"flush-on":"flush-off")<<Clock.elapsed()<<"bytes"<<qulonglong(Job.Value().progress_.bytesDone_)<<"items"<<qulonglong(Job.Value().completedItems_); NextLog+=10000; }
            QTest::qWait(50);
        }
        if(Handle.Value().completion_.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready) Scheduler.Cancel(Handle.Value().id_);
        QTRY_VERIFY_WITH_TIMEOUT(Handle.Value().completion_.wait_for(std::chrono::milliseconds(0))==std::future_status::ready,30000);
        const auto Final=Handle.Value().completion_.get(); const auto Wall=Clock.elapsed();
        QVERIFY(OperationMetrics::Instance().Save(QDir::current().filePath(QString("r23-%1-metrics.json").arg(QString::fromLatin1(QTest::currentDataTag()))),Wall));
        QCOMPARE(Final.snapshot_.state_,JobState::Completed);
        QVERIFY(!Final.writes_);
        QVERIFY(!QDir(Directory.filePath("journal")).exists());
        QDirIterator Files(Directory.filePath(QFileInfo(Source).completeBaseName()),QDir::Files|QDir::Hidden|QDir::System,QDirIterator::Subdirectories);
        std::uint64_t Count=0,Bytes=0; while(Files.hasNext()){Files.next();++Count;Bytes+=Files.fileInfo().size();}
        QCOMPARE(Bytes,Final.snapshot_.progress_.bytesDone_); QCOMPARE(Count,std::uint64_t(23362)); QCOMPARE(Bytes,std::uint64_t(20394591999));
        qInfo()<<"R23 verified"<<(FlushFileContents?"flush-on":"flush-off")<<Wall<<qulonglong(Count)<<qulonglong(Bytes);
    }
    void r18ManyFilesExtractUi()
    {
        QTemporaryDir Directory(QDir::current().filePath("r18-many-source-XXXXXX")); QVERIFY(Directory.isValid());
        QVERIFY(QDir(Directory.path()).mkpath("input")); std::vector<std::string> Names;
        for(int I=0;I<320;++I){const auto Name=QString("input/file-%1.txt").arg(I,4,10,QChar('0'));QFile File(Directory.filePath(Name));QVERIFY(File.open(QIODevice::WriteOnly));File.write("R18 solid batch content");Names.push_back(Name.toStdString());}
        const auto Source=Directory.filePath("many.7z");
        hc::plugins::ArchiveUpdate Create;Create.archive_=Source.toStdString();Create.inputDirectory_=Directory.path().toStdString();Create.add_=std::move(Names);Create.creation_=hc::plugins::ArchiveCreationOptions{5,true};
        auto Backend=hc::plugins::MakeApprovedWcxBackend(hc::plugins::BuiltinSevenZipOptions());auto Created=Backend->Update(std::move(Create),{});QVERIFY2(Created.HasValue(),Created.HasValue()?"":Created.Failure().code_.c_str());
        const auto Before=qgetenv("HC_R18_ARCHIVE_FILE"), Limit=qgetenv("HC_R18_PROBE_MS");
        const auto Restore=qScopeGuard([&]{qputenv("HC_R18_ARCHIVE_FILE",Before);qputenv("HC_R18_PROBE_MS",Limit);});
        qputenv("HC_R18_ARCHIVE_FILE",Source.toUtf8());qputenv("HC_R18_PROBE_MS","30000");r18LargeExtractUi();
    }
    void r18LargeExtractUi()
    {
        const auto Source=qEnvironmentVariable("HC_R18_ARCHIVE_FILE"); if(Source.isEmpty()) QSKIP("Set HC_R18_ARCHIVE_FILE for the real archive regression");
        using namespace hc::operations;
        QTemporaryDir Directory(QDir::current().filePath("r18-real-extract-XXXXXX")); QVERIFY(Directory.isValid());
        const auto Link=Directory.filePath(QFileInfo(Source).fileName());
        const auto LinkNative=QDir::toNativeSeparators(Link), SourceNative=QDir::toNativeSeparators(Source);
        QVERIFY2(CreateHardLinkW(reinterpret_cast<LPCWSTR>(LinkNative.utf16()),reinterpret_cast<LPCWSTR>(SourceNative.utf16()),nullptr),"The test creates only a private hard link, preserving the original archive");
        auto* Panel=panels_.front(); auto* Model=static_cast<CFSModel*>(model(Panel)); Model->SetProviderEnabled(true);
        Panel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()),{}); QTRY_VERIFY(!Model->IsLoading()); emit Panel->sig_NotifyPanelActivated();
        auto* Grid=qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex())); QVERIFY(Grid);
        Grid->BaseView()->deselectAll(); QModelIndex Index;
        for(int R=0;R<Grid->BaseView()->modelController()->getGridRowCount();++R) {
            Grid->BaseView()->setFocusedRowIndex(R); auto At=Grid->BaseView()->focusedRow().modelIndex(0);
            while(const auto* Proxy=qobject_cast<const QAbstractProxyModel*>(At.model())) At=Proxy->mapToSource(At);
            if(Model->GetName(At)==QFileInfo(Source).fileName()){Index=At;break;}
        }
        QVERIFY(Index.isValid()); Grid->EnsureKeyboardFocusOnView();
        auto* Bridge=TyStCommandMgr::GetInstance()->Bridge(); QVERIFY(Bridge); auto& Scheduler=Bridge->FileJobs()->Scheduler();
        QSet<QString> Existing;for(const auto& Job:Scheduler.Snapshots())Existing.insert(QString::fromStdString(Job.id_));
        bool Seen=false;QTimer::singleShot(0,main_,[&]{
            auto* Dialog=static_cast<hc::ui::ArchiveExtractDialog*>(main_->findChild<QDialog*>("hc.archive.extract"));if(!Dialog)return;Seen=true;
            Dialog->findChild<QLineEdit*>("hc.archive.extract.path")->setText(".");
            Seen=SaveR9Dialog(*Dialog,"r18-many-extract");
            QTest::keyClick(Dialog->findChild<QLineEdit*>("hc.archive.extract.path"),Qt::Key_Return);
        });
        hc::operations::OperationMetrics::Instance().Reset(); QElapsedTimer TotalClock; TotalClock.start();
        Bridge->PromptArchive(true,Index); QVERIFY(Seen);
        std::string Id;for(const auto& Job:Scheduler.Snapshots())if(!Existing.contains(QString::fromStdString(Job.id_)))Id=Job.id_;QVERIFY(!Id.empty());
        Bridge->FileJobs()->Show(); auto* Manager=main_->findChild<QDialog*>("hc.jobManager"); QVERIFY(Manager);
        QVERIFY(SaveR9Dialog(*Manager,"r18-many-running"));
        QElapsedTimer Clock; Clock.start();int NextLog=0;
        const auto Limit=qEnvironmentVariableIntValue("HC_R18_PROBE_MS");
        while(Clock.elapsed()<(Limit?Limit:600000)){
            const auto Job=Scheduler.Snapshot(Id); QVERIFY(Job.HasValue());
            if(Clock.elapsed()>=NextLog){qInfo()<<"R18"<<Clock.elapsed()<<"state"<<int(Job.Value().state_)<<"bytes"<<qulonglong(Job.Value().progress_.bytesDone_)<<"current"<<QString::fromStdString(Job.Value().progress_.currentItem_)<<"completed"<<qulonglong(Job.Value().completedItems_);NextLog+=5000;}
            if(IsTerminal(Job.Value().state_))break;
            if(Job.Value().interaction_){qInfo()<<"R18 interaction"<<QString::fromStdString(Job.Value().interaction_->reason_);Scheduler.Control({Id},JobControl::Cancel);break;}
            QTest::qWait(50);
        }
        const auto Final=Scheduler.Snapshot(Id); QVERIFY(Final.HasValue());
        const bool Done=Final.Value().state_==JobState::Completed;
        if(!IsTerminal(Final.Value().state_)){Scheduler.Control({Id},JobControl::Cancel);QTRY_VERIFY_WITH_TIMEOUT(IsTerminal(Scheduler.Snapshot(Id).Value().state_),30000);}
        QVERIFY(hc::operations::OperationMetrics::Instance().Save(QDir::current().filePath("r19-operation-metrics.json"),TotalClock.elapsed()));
        qInfo()<<"R18 result"<<Clock.elapsed()<<QString::fromStdString(Final.Value().error_?Final.Value().error_->code_:"none");
        QTRY_VERIFY(!Bridge->FileJobs()->IsUiPending(Id));
        QVERIFY(SaveR9Dialog(*Manager,"r18-many-completed")); Manager->hide();
        for(auto* D:main_->findChildren<QDialog*>("hc.fileJobs"))D->hide();
        QVERIFY2(Done,"Large archive extraction did not complete; inspect R18 progress and host errors");
        const auto OutputRoot=Directory.filePath(QFileInfo(Source).completeBaseName());QDirIterator OutputFiles(OutputRoot,QDir::Files|QDir::Hidden|QDir::System,QDirIterator::Subdirectories);
        std::uint64_t Total=0, Files=0;while(OutputFiles.hasNext()){OutputFiles.next();Total+=OutputFiles.fileInfo().size();++Files;}
        QVERIFY(Final.Value().progress_.bytesTotal_); QCOMPARE(Total,*Final.Value().progress_.bytesTotal_); QVERIFY(Final.Value().progress_.plannedItems_);
        QCOMPARE(Total,Final.Value().progress_.bytesDone_);qInfo()<<"R18 verified output files"<<qulonglong(Files)<<"bytes"<<qulonglong(Total);
        if(QFileInfo(Source).size()>1024ll*1024*1024) QVERIFY(Files>10000);
        if(QFileInfo(Source).fileName()=="many.7z") {
            QVERIFY(Final.Value().completedItems_>320);
            for(int I=0;I<320;++I){QFile File(QDir(Directory.path()).filePath(QString("many/input/file-%1.txt").arg(I,4,10,QChar('0'))));QVERIFY(File.open(QIODevice::ReadOnly));QCOMPARE(File.readAll(),QByteArray("R18 solid batch content"));}
        }
    }
    void r16ArchiveCommandsUi_data() { QTest::addColumn<QString>("Format"); QTest::addColumn<bool>("Encrypted"); QTest::newRow("zip") << QString("zip") << false; QTest::newRow("7z") << QString("7z") << false; QTest::newRow("encrypted-7z") << QString("7z") << true; }
    void r16ArchiveCommandsUi()
    {
        QFETCH(QString, Format); QFETCH(bool, Encrypted); using namespace hc::operations; using namespace hc::ui;
        auto& Settings = hc::settings::AppSettings(); const auto Before = Settings.Object(); const auto Workspace = main_->CaptureWorkspace();
        const auto RestoreSettings = qScopeGuard([&] { Settings.Replace(Before); Settings.Commit(); TyStCommandMgr::GetInstance()->Refresh(); main_->RestoreWorkspace(Workspace); });
        Settings.Put(hc::settings::Section::Shortcuts,QJsonObject{}); TyStCommandMgr::GetInstance()->Refresh();
        QTemporaryDir Directory; QVERIFY(Directory.isValid()); QVERIFY(QDir(Directory.path()).mkpath("input/empty"));
        QFile Original(Directory.filePath("input/한글.txt")); QVERIFY(Original.open(QIODevice::WriteOnly)); Original.write("R16 archive content"); Original.close();
        QFile Ordinary(Directory.filePath("ordinary.txt")); QVERIFY(Ordinary.open(QIODevice::WriteOnly)); Ordinary.write("plain"); Ordinary.close();
        if(localOnly_) for(auto* Other:panels_) {
            if(auto* Error=Other->findChild<QMessageBox*>("hc.navigationError"))Error->reject();
            Other->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()),{}); QTRY_VERIFY(!model(Other)->IsLoading());
        }
        main_->raise(); main_->activateWindow(); QVERIFY(QTest::qWaitForWindowActive(main_));
        auto* Panel = panels_.front(); auto* Model = static_cast<CFSModel*>(model(Panel)); Model->SetProviderEnabled(true);
        Panel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()),{}); QTRY_VERIFY(!Model->IsLoading()); emit Panel->sig_NotifyPanelActivated();
        auto* Grid = qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex())); QVERIFY(Grid);
        auto* Bridge = TyStCommandMgr::GetInstance()->Bridge(); QVERIFY(Bridge); auto& Scheduler = Bridge->FileJobs()->Scheduler();
        const auto Focus = [&](const QString& Name) -> QModelIndex {
            Grid->BaseView()->deselectAll();
            for (int Row = 0; Row < Grid->BaseView()->modelController()->getGridRowCount(); ++Row) {
                Grid->BaseView()->setFocusedRowIndex(Row); auto Index = Grid->BaseView()->focusedRow().modelIndex(0);
                while (const auto Proxy = qobject_cast<const QAbstractProxyModel*>(Index.model())) Index = Proxy->mapToSource(Index);
                if (Model->GetName(Index) == Name) { Grid->EnsureKeyboardFocusOnView(); emit Panel->sig_NotifyPanelActivated(); return Index; }
            }
            return {};
        };
        const auto LatestJob = [&](const QSet<QString>& Existing) { std::string Id; for (const auto& Job : Scheduler.Snapshots()) if (!Existing.contains(QString::fromStdString(Job.id_))) Id = Job.id_; return Id; };
        const auto ExistingJobs = [&] { QSet<QString> Ids; for (const auto& Job : Scheduler.Snapshots()) Ids.insert(QString::fromStdString(Job.id_)); return Ids; };
        const auto Finish = [&](const std::string& Id) {
            QElapsedTimer Clock; Clock.start(); while (Clock.elapsed() < 40000) {
                const auto Snapshot = Scheduler.Snapshot(Id); if (!Snapshot.HasValue()) return false;
                if (IsTerminal(Snapshot.Value().state_)) { if (Snapshot.Value().state_ != JobState::Completed) qWarning() << "R16 job" << (Snapshot.Value().error_ ? Snapshot.Value().error_->code_ : "failed"); return Snapshot.Value().state_ == JobState::Completed; }
                if (Snapshot.Value().interaction_ && Snapshot.Value().interaction_->kind_ == InteractionKind::Password)
                    Scheduler.Reply(Id, {Snapshot.Value().interaction_->id_,InteractionChoice::Confirm,"r17-password"});
                QTest::qWait(25);
            } return false;
        };
        const auto BaseName = Format=="zip" ? QString("archive") : QString("created"); const auto Output = Directory.filePath(BaseName+"."+Format); auto Index = Focus("input"); QVERIFY(Index.isValid());
        auto Prior = ExistingJobs(); bool Seen = false;
        QTimer::singleShot(0,main_,[&] { auto* Dialog = main_->findChild<QDialog*>("hc.archive.create"); if (!Dialog) return; Seen = true;
            auto* CreatePath=Dialog->findChild<QLineEdit*>("hc.archive.create.path"); QCOMPARE(Dialog->focusWidget(),CreatePath); CreatePath->setText(Format=="zip" ? QString(".") : "./created."+Format); Dialog->findChild<QComboBox*>("hc.archive.create.format")->setCurrentIndex(Format=="7z"?0:1);
            QVERIFY(!Dialog->findChild<QLabel*>("hc.archive.create.preview"));
            QTRY_VERIFY(Dialog->findChild<QPushButton*>("hc.archive.create.settings")->isEnabled());
            QTimer::singleShot(0,Dialog,[&] { auto* Settings=Dialog->findChild<QDialog*>("hc.archive.settings."+Format); QVERIFY(Settings); Settings->findChild<QCheckBox*>("hc.archive.settings.encrypt")->setChecked(Encrypted); Settings->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click(); });
            Dialog->findChild<QPushButton*>("hc.archive.create.settings")->click();
            QVERIFY(SaveR9Dialog(*Dialog,"r17-compress-"+Format+(Encrypted?"-encrypted":""))); QTRY_VERIFY(Dialog->findChild<QPushButton*>("hc.archive.create.queue")->isEnabled()); Dialog->findChild<QPushButton*>("hc.archive.create.queue")->click();
        });
        QTest::keyClick(QApplication::focusWidget(),Qt::Key_A,Qt::ControlModifier); QVERIFY(Seen);
        auto Id = LatestJob(Prior); QVERIFY(!Id.empty()); QVERIFY(Scheduler.Snapshot(Id).Value().deferred_); QVERIFY(!QFileInfo::exists(Output)); QVERIFY(Scheduler.Control({Id},JobControl::Start).HasValue()); QVERIFY(Finish(Id));
        QTRY_VERIFY(!Bridge->FileJobs()->IsUiPending(Id));
        if (!Encrypted) QVERIFY(QFile::copy(Output,Directory.filePath("second."+Format)));
        Model->Refresh(); QTRY_VERIFY(!Model->IsLoading() && names(Model).contains(BaseName+"."+Format));
        for(auto* Progress:main_->findChildren<QDialog*>("hc.fileJobs"))Progress->hide(); main_->raise(); main_->activateWindow(); QTest::qWait(40);
        Index=Focus(BaseName+"."+Format); QVERIFY(Index.isValid()); QTRY_VERIFY(Bridge->Action("hc.archive.extract")->isEnabled());
        if (!Encrypted) {
            Grid->BaseView()->selectRow(Grid->BaseView()->focusedRow().rowIndex(),Qtitan::Select);
            for (int R=0;R<Grid->BaseView()->modelController()->getGridRowCount();++R) {
                auto Source=Grid->BaseView()->getRow(R).modelIndex(0); while(const auto Proxy=qobject_cast<const QAbstractProxyModel*>(Source.model()))Source=Proxy->mapToSource(Source);
                if(Model->GetName(Source)=="second."+Format)Grid->BaseView()->selectRow(R,Qtitan::Select);
            }
            QCOMPARE(Grid->BaseView()->selection()->selectedRowIndexes().size(),2);
        }
        Prior=ExistingJobs(); Seen=false; const auto Target=Format=="zip" ? Directory.path() : Directory.filePath("unpacked");
        const auto RelativeTarget=Format=="zip" ? QString(".") : Encrypted ? "../"+QFileInfo(Directory.path()).fileName()+"/unpacked" : QString(".\\unpacked");
        QTimer::singleShot(0,main_,[&] { auto* Dialog=static_cast<ArchiveExtractDialog*>(main_->findChild<QDialog*>("hc.archive.extract")); if(!Dialog)return; Seen=true;
            auto* Path=Dialog->findChild<QLineEdit*>("hc.archive.extract.path"); QCOMPARE(Dialog->focusWidget(),Path); Path->setText(RelativeTarget); Dialog->findChild<QCheckBox*>("hc.archive.extract.queued")->setChecked(true);
            Path->setFocus(); QTest::keyClick(Path,Qt::Key_A,Qt::ControlModifier); QCOMPARE(Path->selectedText(),RelativeTarget);
            QVERIFY(SaveR9Dialog(*Dialog,"r17-extract-"+Format+(Encrypted?"-encrypted":""))); QTest::keyClick(Dialog->findChild<QLineEdit*>("hc.archive.extract.path"),Qt::Key_Return);
        });
        QTest::keyClick(QApplication::focusWidget(),Qt::Key_X,Qt::ControlModifier); QVERIFY(Seen);
        Id=LatestJob(Prior); QVERIFY(!Id.empty()); QVERIFY(Scheduler.Snapshot(Id).Value().deferred_); QVERIFY(Scheduler.Control({Id},JobControl::Start).HasValue()); QVERIFY(Finish(Id));
        const auto Unpacked=QDir(Target).filePath(BaseName+"/input/한글.txt"); QFile Result(Unpacked); QVERIFY2(Result.open(QIODevice::ReadOnly),qPrintable(Unpacked)); QCOMPARE(Result.readAll(),QByteArray("R16 archive content")); Result.close(); QVERIFY(QDir(QDir(Target).filePath(BaseName+"/input/empty")).exists()); QVERIFY(QFileInfo::exists(Output));
        if(!Encrypted) QVERIFY(QFileInfo::exists(QDir(Target).filePath("second/input/한글.txt")));
        QTRY_VERIFY(!Bridge->FileJobs()->IsUiPending(Id));
        QMainOpts Options(main_); Options.show(); Options.LoadSettings(); auto* Table=Options.findChild<QTableWidget*>("hc.keys.commands"); QVERIFY(Table);
        for (const auto& Command : {"hc.archive.create","hc.archive.extract","hc.navigate.enterFile"}) {
            bool Found=false; for(int R=0;R<Table->rowCount();++R) if(Table->item(R,0)->data(Qt::UserRole).toString()==Command) {Found=true; QVERIFY(!Table->item(R,1)->text().isEmpty());}
            QVERIFY(Found);
        }
        auto* Categories=Options.findChild<QListWidget*>("lstOptCate"); for(int R=0;R<Categories->count();++R)if(Categories->item(R)->text()=="키보드")Categories->setCurrentRow(R);
        QVERIFY(SaveR9Dialog(Options,"r17-keyboard")); Options.reject();
        if(!Encrypted) {
            Index=Focus("ordinary.txt"); QVERIFY(Index.isValid()); const auto Location=Model->WorkspaceLocation();
            QKeyEvent Plain(QEvent::KeyPress,Qt::Key_PageDown,Qt::ControlModifier); QVERIFY(TyStCommandMgr::GetInstance()->ProcessKeyPressEvent(&Plain,Index)); QTest::qWait(80); QCOMPARE(Model->WorkspaceLocation(),Location);
            Index=Focus(BaseName+"."+Format); QVERIFY(Index.isValid()); QTest::keyClick(QApplication::focusWidget(),Qt::Key_PageDown,Qt::ControlModifier);
            QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading() && Model->WorkspaceLocation().kind_==hc::storage::LocatorKind::Archive && names(Model).contains("input"),15000);
            QVERIFY(SaveR9Dialog(*main_,"r17-enter-"+Format));
            for(auto* Progress:main_->findChildren<QDialog*>("hc.fileJobs"))Progress->hide(); main_->raise(); main_->activateWindow(); QTest::qWait(40);
            Index=Focus("input"); QVERIFY(Index.isValid()); QTRY_VERIFY(Bridge->Action("hc.archive.extract")->isEnabled());
            Prior=ExistingJobs(); Seen=false; const auto InsideTarget=Directory.filePath("inside");
            QTimer::singleShot(0,main_,[&] {
                auto* Dialog=static_cast<ArchiveExtractDialog*>(main_->findChild<QDialog*>("hc.archive.extract")); if(!Dialog)return; Seen=true;
                auto* Separate=Dialog->findChild<QCheckBox*>("hc.archive.extract.separate"); QVERIFY(!Separate->isEnabled()); QVERIFY(!Separate->isChecked());
                Dialog->findChild<QLineEdit*>("hc.archive.extract.path")->setText("./inside"); Dialog->findChild<QCheckBox*>("hc.archive.extract.queued")->setChecked(true);
                QVERIFY(SaveR9Dialog(*Dialog,"r17-extract-inside-"+Format)); QTest::keyClick(Dialog->findChild<QLineEdit*>("hc.archive.extract.path"),Qt::Key_Return);
            });
            QTest::keyClick(QApplication::focusWidget(),Qt::Key_X,Qt::ControlModifier); QVERIFY(Seen);
            Id=LatestJob(Prior); QVERIFY(!Id.empty()); QVERIFY(Scheduler.Control({Id},JobControl::Start).HasValue()); QVERIFY(Finish(Id));
            QFile InsideResult(QDir(InsideTarget).filePath("input/한글.txt")); QVERIFY(InsideResult.open(QIODevice::ReadOnly)); QCOMPARE(InsideResult.readAll(),QByteArray("R16 archive content"));
            QVERIFY(QDir(QDir(InsideTarget).filePath("input/empty")).exists()); QVERIFY(QFileInfo::exists(Output)); QTRY_VERIFY(!Bridge->FileJobs()->IsUiPending(Id));
        }
        for(auto* Dialog:main_->findChildren<QDialog*>("hc.fileJobs"))Dialog->hide();
    }
    void sevenZipCreateUi_data() { QTest::addColumn<QString>("Format"); QTest::newRow("7z") << QString("7z"); QTest::newRow("zip") << QString("zip"); }
    void sevenZipCreateUi()
    {
        QFETCH(QString, Format);
        using namespace hc::operations;
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        const auto Output = Directory.filePath("created." + Format);
        { QFile File(Directory.filePath("한글.txt")); QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write("ZIP UI input"), 12); }
        auto* Panel = panels_.front(); auto* Model = static_cast<CFSModel*>(model(Panel)); Model->SetProviderEnabled(true);
        Panel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {}); QTRY_VERIFY(!Model->IsLoading());
        auto* Grid = qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex())); QVERIFY(Grid);
        const auto Focus = [&](const QString& Name) {
            for (int Row = 0; Row < Grid->BaseView()->modelController()->getGridRowCount(); ++Row) {
                Grid->BaseView()->setFocusedRowIndex(Row); auto Index = Grid->BaseView()->focusedRow().modelIndex(0);
                while (const auto Proxy = qobject_cast<const QAbstractProxyModel*>(Index.model())) Index = Proxy->mapToSource(Index);
                if (Model->GetName(Index) == Name) { Grid->EnsureKeyboardFocusOnView(); Panel->sig_NotifyPanelActivated(); return true; }
            }
            return false;
        };
        QVERIFY(Focus("한글.txt"));
        auto* Create = main_->findChild<QAction*>("hc.menu.archive.create"); QVERIFY(Create);
        auto& Scheduler = TyStCommandMgr::GetInstance()->Bridge()->FileJobs()->Scheduler();
        QSet<QString> ExistingJobs; for (const auto& Job : Scheduler.Snapshots()) ExistingJobs.insert(QString::fromStdString(Job.id_));
        bool DialogSeen = false;
        QTimer::singleShot(0, main_, [&] {
            auto* Dialog = main_->findChild<QDialog*>("hc.archive.create"); if (!Dialog) return;
            const auto CloseDialog = qScopeGuard([Dialog] { Dialog->accept(); });
            DialogSeen = true;
            auto* Path = Dialog->findChild<QLineEdit*>("hc.archive.create.path"); Path->setText(Output);
            Dialog->findChild<QComboBox*>("hc.archive.create.format")->setCurrentIndex(Format == "7z" ? 0 : 1);
            QVERIFY(Dialog->rect().contains(Path->mapTo(Dialog, QPoint(0, 0))));
            QVERIFY(Dialog->grab().save(QDir::current().filePath(QString("archive-create-%1.png").arg(Format))));
            QTRY_VERIFY(Dialog->findChild<QPushButton*>("hc.archive.create.queue")->isEnabled()); Dialog->findChild<QPushButton*>("hc.archive.create.queue")->click();
        });
        Create->trigger(); QVERIFY(DialogSeen);
        QTRY_VERIFY_WITH_TIMEOUT(Scheduler.Snapshots().size() > size_t(ExistingJobs.size()), 5000);
        std::string Job; for (const auto& Value : Scheduler.Snapshots()) if (!ExistingJobs.contains(QString::fromStdString(Value.id_))) Job = Value.id_;
        QVERIFY(!Job.empty()); QTest::qWait(100); QVERIFY(Scheduler.Snapshot(Job).Value().deferred_); QVERIFY(!QFile::exists(Output));
        auto* StartQueue = main_->findChild<QPushButton*>("hc.jobs.startQueue"); QVERIFY(StartQueue); StartQueue->click();
        QTRY_VERIFY_WITH_TIMEOUT(IsTerminal(Scheduler.Snapshot(Job).Value().state_), 30000);
        const auto Snapshot = Scheduler.Snapshot(Job).Value();
        QVERIFY2(Snapshot.state_ == JobState::Completed, Snapshot.error_ ? Snapshot.error_->code_.c_str() : "archive job failed");
        for (auto* Dialog : main_->findChildren<QDialog*>("hc.fileJobs")) Dialog->hide();
        QTRY_VERIFY(!Model->IsLoading()); Model->Refresh(); QTRY_VERIFY(!Model->IsLoading() && names(Model).contains("created." + Format));
        QVERIFY(Focus("created." + Format));
        auto Selected = Grid->BaseView()->focusedRow().modelIndex(0); Panel->ReturnOnCurrentTab(Selected);
        QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading() && names(Model).contains("한글.txt"), 10000);
        using namespace hc::storage;
        const auto Pack = dynamic_cast<nsHC::CFSPack*>(Model->GetRoot().get()); QVERIFY(Pack); QVERIFY(Pack->GetWCX()->WritePolicy().HasValue());
        auto Reader = Model->SnapshotProvider(); auto Root = Model->GetRoot()->ReadLocation("/"); auto At = Root; At.nesting_.back().entry_ = "/한글.txt";
        auto Entry = Reader->Resolve(At, {}).get(); QVERIFY(Entry.HasValue());
        const auto Options = hc::plugins::BuiltinSevenZipOptions();
        auto Batch = BeginWcxMutation(Options.binaryPath_.toStdString(), Root, Reader, {}, {}, {}, {}, hc::plugins::MakeApprovedWcxBackend(Options));
        QVERIFY2(Batch.HasValue(), Batch.HasValue() ? "" : Batch.Failure().code_.c_str());
        auto Prepared = Batch.Value()->Prepare({{ArchiveChangeKind::Rename, "/한글.txt", "/renamed.txt", {}, Entry.Value().id_}}, {});
        QVERIFY2(Prepared.HasValue(), Prepared.HasValue() ? "" : Prepared.Failure().code_.c_str());
        auto Committed = Batch.Value()->Commit({}); QVERIFY2(Committed.HasValue(), Committed.HasValue() ? "" : Committed.Failure().code_.c_str());
        Model->Refresh(); QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading() && names(Model).contains("renamed.txt"), 10000);
        QVERIFY(!GetModuleHandleW(L"7z.dll")); Panel->ChangeCurrentPath(unc_, {}); QTRY_VERIFY(!Model->IsLoading());
    }
    void isoPluginUi()
    {
        QFETCH(int, ProfileIndex);
        using namespace hc::operations;
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        const auto Output = Directory.filePath(ProfileIndex==3 ? "image.iso" : "created.iso");
        { QFile File(Directory.filePath("한글.txt")); QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write("ISO UI input"), 12); }
        auto* Panel = panels_.front(); auto* Model = static_cast<CFSModel*>(model(Panel)); Model->SetProviderEnabled(true);
        Panel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {}); QTRY_VERIFY(!Model->IsLoading());
        auto* Grid = qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex())); QVERIFY(Grid);
        const auto Focus = [&](const QString& Name) {
            for (int Row = 0; Row < Grid->BaseView()->modelController()->getGridRowCount(); ++Row) {
                Grid->BaseView()->setFocusedRowIndex(Row); auto Index = Grid->BaseView()->focusedRow().modelIndex(0);
                while (const auto Proxy = qobject_cast<const QAbstractProxyModel*>(Index.model())) Index = Proxy->mapToSource(Index);
                if (Model->GetName(Index) == Name) { Grid->EnsureKeyboardFocusOnView(); Panel->sig_NotifyPanelActivated(); return true; }
            }
            return false;
        };
        QVERIFY(Focus("한글.txt"));
        auto* Create = main_->findChild<QAction*>("hc.menu.iso.create"); QVERIFY(Create);
        auto& Scheduler = TyStCommandMgr::GetInstance()->Bridge()->FileJobs()->Scheduler();
        QSet<QString> ExistingJobs; for (const auto& Job : Scheduler.Snapshots()) ExistingJobs.insert(QString::fromStdString(Job.id_));
        bool DialogSeen = false;
        QTimer::singleShot(0, main_, [&] {
            auto* Dialog = main_->findChild<QDialog*>("hc.archive.create"); if (!Dialog) return;
            const auto CloseDialog = qScopeGuard([Dialog] { if(Dialog->isVisible()) Dialog->reject(); });
            DialogSeen = true;
            auto* Path = Dialog->findChild<QLineEdit*>("hc.archive.create.path"); Path->setText(Output);
            QCOMPARE(Dialog->findChild<QComboBox*>("hc.archive.create.format")->currentData().toString(), QString("iso"));
            QTRY_VERIFY(Dialog->findChild<QPushButton*>("hc.archive.create.settings")->isEnabled());
            QTimer::singleShot(0,Dialog,[&] { auto* Settings=Dialog->findChild<QDialog*>("hc.archive.settings.iso"); QVERIFY(Settings); Settings->findChild<QComboBox*>("hc.archive.settings.profile")->setCurrentIndex(ProfileIndex); Settings->findChild<QCheckBox*>("hc.archive.settings.share")->setChecked(true); Settings->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click(); });
            Dialog->findChild<QPushButton*>("hc.archive.create.settings")->click();
            QVERIFY(Dialog->grab().save(QDir::current().filePath(QString("iso-create-%1.png").arg(ProfileIndex))));
            QTRY_VERIFY(Dialog->findChild<QPushButton*>("hc.archive.create.submit")->isEnabled()); Dialog->findChild<QPushButton*>("hc.archive.create.submit")->click();
        });
        Create->trigger(); QVERIFY(DialogSeen);
        QTRY_VERIFY_WITH_TIMEOUT(Scheduler.Snapshots().size() > size_t(ExistingJobs.size()), 5000);
        std::string Job; for (const auto& Value : Scheduler.Snapshots()) if (!ExistingJobs.contains(QString::fromStdString(Value.id_))) Job = Value.id_;
        QVERIFY(!Job.empty());
        QTRY_VERIFY_WITH_TIMEOUT(IsTerminal(Scheduler.Snapshot(Job).Value().state_), 30000);
        const auto Snapshot = Scheduler.Snapshot(Job).Value();
        QVERIFY2(Snapshot.state_ == JobState::Completed, Snapshot.error_ ? Snapshot.error_->code_.c_str() : "ISO job failed");
        for (auto* Dialog : main_->findChildren<QDialog*>("hc.fileJobs")) Dialog->hide();
        QTRY_VERIFY(!Model->IsLoading()); Model->Refresh(); QTRY_VERIFY(!Model->IsLoading() && names(Model).contains(QFileInfo(Output).fileName()));
        QVERIFY(Focus(QFileInfo(Output).fileName()));
        auto* Inspect = main_->findChild<QAction*>("hc.menu.iso.inspect"); QVERIFY(Inspect); Inspect->trigger();
        auto* Dialog = main_->findChild<QDialog*>("hc.iso.inspect"); QVERIFY(Dialog);
        auto* Status = Dialog->findChild<QLabel*>("hc.iso.inspect.status"); QVERIFY(Status);
        QTRY_VERIFY_WITH_TIMEOUT(Status->text().contains("구조 확인 완료"), 10000); QVERIFY(Status->text().contains("UDF 1.02"));
        auto* Table = Dialog->findChild<QTableWidget*>("hc.iso.inspect.entries"); QVERIFY(Table); QCOMPARE(Table->rowCount(), 1); QCOMPARE(Table->item(0, 0)->text(), QString("한글.txt"));
        QCOMPARE(Status->text().contains("Joliet"), ProfileIndex == 2);
        QVERIFY(Dialog->grab().save(QDir::current().filePath(QString("iso-inspect-%1.png").arg(ProfileIndex))));
        Dialog->findChild<QPushButton*>("hc.iso.inspect.readAll")->click(); QTRY_VERIFY_WITH_TIMEOUT(Status->text().contains("전체 읽기 검사 완료"), 10000);
        Dialog->close(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QTRY_VERIFY(!Model->IsLoading()); QVERIFY(Focus(QFileInfo(Output).fileName()));
        auto Selected = Grid->BaseView()->focusedRow().modelIndex(0);
        Panel->ReturnOnCurrentTab(Selected);
        QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading() && Model->GetRoot()->ReadLocation(Model->GetCurrentPathWithRoot()).kind_ == hc::storage::LocatorKind::Archive, 10000);
        QVERIFY(names(Model).contains("한글.txt"));
        QCOMPARE(Model->SnapshotProvider()->Id().value_, std::string("iso"));
        QCOMPARE(Model->SnapshotProvider()->Capabilities().read_, hc::storage::ReadAccess::RandomAccess);
        const auto PreviousProfile = Grid->ColumnProfile(); auto PreviewProfile = PreviousProfile;
        PreviewProfile.columns_[1].fieldId_ = "file.allocatedSize"; PreviewProfile.columns_[1].format_ = {};
        QVERIFY(Grid->ApplyColumnProfile(PreviewProfile));
        {
            QMainOpts Options(main_); Options.SetListView(Grid); Options.ShowListSettingsPage(); Options.show(); QCoreApplication::processEvents();
            Options.findChild<QComboBox*>("hc.columns.sampleMode")->setCurrentIndex(1);
            Options.findChild<QCheckBox*>("hc.columns.extractSamples")->setChecked(true);
            Options.findChild<QPushButton*>("hc.columns.calculate")->click();
            auto* Preview = Options.findChild<QTableWidget*>("hc.columns.preview"); QCOMPARE(Preview->rowCount(), 1);
            QTRY_VERIFY2_WITH_TIMEOUT(Preview->item(0, 1)->text().toULongLong() > 0, qPrintable(Preview->item(0, 1)->toolTip()), 10000);
            Options.reject();
        }
        QVERIFY(Grid->ApplyColumnProfile(PreviousProfile));
        // Keep a native read session open across the compatibility writer's
        // atomic replacement. Old IDs must fail and the refreshed panel reload.
        {
            using namespace hc::storage;
            auto Reader = Model->SnapshotProvider(); auto Root = Model->GetRoot()->ReadLocation("/");
            auto At = Root; At.nesting_.back().entry_ = "/한글.txt";
            auto Entry = Reader->Resolve(At, {}).get(); QVERIFY(Entry.HasValue());
            auto Stream = Reader->OpenRead(Entry.Value().id_, {}).get(); QVERIFY(Stream.HasValue());
            const auto Options = hc::plugins::BuiltinIsoOptions();
            auto Batch = BeginWcxMutation(Options.binaryPath_.toStdString(), Root, Reader, {}, {}, {}, {}, hc::plugins::MakeApprovedWcxBackend(Options));
            QVERIFY2(Batch.HasValue(), Batch.HasValue() ? "" : Batch.Failure().code_.c_str());
            auto Prepared = Batch.Value()->Prepare({{ArchiveChangeKind::Rename, "/한글.txt", "/renamed.txt", {}, Entry.Value().id_}}, {});
            QVERIFY2(Prepared.HasValue(), Prepared.HasValue() ? "" : Prepared.Failure().code_.c_str());
            auto Committed = Batch.Value()->Commit({});
            QVERIFY2(Committed.HasValue(), Committed.HasValue() ? "" : Committed.Failure().code_.c_str());
            auto Old = Stream.Value()->Read(8, {}).get(); QVERIFY(!Old.HasValue()); QCOMPARE(Old.Failure().category_, ErrorCategory::StaleRevision);
            Model->Refresh(); QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading() && names(Model).contains("renamed.txt"), 10000);
        }
        qInfo() << "Built-in ISO create, inspect, native read, WCX mutation, archive navigation and extracted column preview passed";
    }

    void shareCapacityStatusUi_data()
    {
        QTest::addColumn<bool>("Enabled");
        QTest::newRow("legacy") << false;
        QTest::newRow("provider") << true;
    }

    void shareCapacityStatusUi()
    {
        QFETCH(bool, Enabled);
        const auto Workspace = main_->CaptureWorkspace();
        const auto Restore = qScopeGuard([&] { main_->RestoreWorkspace(Workspace); });
        QTemporaryDir Local;
        QVERIFY(Local.isValid());
        for (auto* Panel : panels_)
        {
            auto* Model = static_cast<CFSModel*>(model(Panel));
            Panel->ChangeCurrentPath(QDir::toNativeSeparators(Local.path()), {});
            QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 15000);
            Model->SetProviderEnabled(Enabled);
            QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 15000);
            auto* Label = Panel->findChild<QLabel*>("lblVolumeStatus");
            QVERIFY(Label);
            auto Path = unc_;
            for (int Pass = 0; Pass < 2; ++Pass)
            {
                Panel->ChangeCurrentPath(Path, {});
                QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 15000);
                QVERIFY(!Model->LastNavigationError());
                QVERIFY(Model->GetRoot()->IsRemote());
                QTRY_VERIFY_WITH_TIMEOUT(Label->text().contains(QStringLiteral("여유")), 15000);
                ULARGE_INTEGER Available{}, Total{};
                const auto Native = QDir::toNativeSeparators(Path) + '\\';
                QVERIFY(GetDiskFreeSpaceExW(Native.toStdWString().c_str(), &Available, &Total, nullptr));
                const auto Style = static_cast<TyEnSizeStyle>(hc::settings::AppSettings().SizeStyle(true));
                QVERIFY(Label->text().contains(GetFormattedSizeText(Total.QuadPart, Style)));
                QCOMPARE(Label->toolTip(), Label->text());
                qInfo() << "Capacity:" << Path << Label->text() << "available bytes:" << Available.QuadPart << "total bytes:" << Total.QuadPart;
                QVERIFY(Panel->grab().save(QCoreApplication::applicationDirPath()
                    + QString("/share-capacity-%1-%2-%3.png").arg(Enabled ? "provider" : "legacy").arg(panels_.indexOf(Panel)).arg(Pass)));
                if (Pass == 0)
                {
                    // Prefer an existing child; never create data on the user's share.
                    for (int Row = 0; Row < Model->rowCount(); ++Row)
                    {
                        const auto Item = Model->GetFileInfo(Model->index(Row, 0));
                        if (Item && Item->Name_ != ".." && (Item->Attributes_ & FILE_ATTRIBUTE_DIRECTORY))
                        { Path = unc_ + '\\' + Item->Name_; break; }
                    }
                }
            }
            // Invalidate an in-flight share result before moving back to a local volume.
            QVERIFY(QMetaObject::invokeMethod(Panel, "processVolumeStatusText", Qt::DirectConnection,
                Q_ARG(nsHC::TySpFileSystem, Model->GetRoot())));
            Panel->ChangeCurrentPath(QDir::toNativeSeparators(Local.path()), {});
            QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 15000);
            QTRY_VERIFY_WITH_TIMEOUT(Label->text().contains(QStringLiteral("여유")) && !Label->text().contains(server_), 15000);
            QTest::qWait(200);
            QVERIFY(!Label->text().contains(server_));
        }
    }

    void networkDriveStartup()
    {
        const bool Synthetic = qEnvironmentVariableIsSet("HC_TEST_DISCONNECTED_DRIVE");
        const auto Root = Synthetic ? DriveProbeFixture::Root() : qEnvironmentVariable("HC_TEST_MAPPED_DRIVE");
        if (Root.isEmpty()) QSKIP("Run the network-startup fixture or select an existing mapped drive");
        QVERIFY2(Root.size() == 2 && Root[1] == ':', "Select a mapped drive or the disconnected-drive fixture");
        qInfo() << "Startup to panel initialization (ms):" << StartupTimer.elapsed()
                << "mapped drive:" << Root << "synthetic:" << Synthetic;
        if (Synthetic)
        {
            qInfo() << "Synthetic metadata calls:" << DriveProbeFixture::MetadataCalls()
                    << "on GUI:" << DriveProbeFixture::GuiMetadataCalls();
            QCOMPARE(DriveProbeFixture::MetadataCalls(), 0);
            QCOMPARE(DriveProbeFixture::AttributeCalls(), 0);
        }
        for (auto* Panel : panels_)
        {
            auto* Combo = Panel->findChild<QComboBox*>("cbxVolume");
            QVERIFY(Combo);
            nsHC::TySpFileSystem Mapped;
            for (int Index = 0; Index < Combo->count(); ++Index)
            {
                auto Fs = Combo->itemData(Index).value<nsHC::TySpFileSystem>();
                if (Fs && Fs->GetRoot().compare(Root, Qt::CaseInsensitive) == 0) Mapped = Fs;
            }
            QVERIFY2(Mapped, "A disconnected drive must remain visible in both panels");
            QVERIFY(Mapped->IsRemote());
            QVERIFY(!model(Panel)->GetRoot()->IsRemote());
            // The native writer distinguishes drive-letter and UNC syntax.
            QCOMPARE(Mapped->ReadLocation(Root + '\\').kind_, hc::storage::LocatorKind::Local);
            QVERIFY(Mapped->ReadProvider());
            for (const auto& Path : QStringList{Root.toLower() + "/folder", "\\\\?\\" + Root + "\\folder"})
            {
                auto Fs = nsHC::TySpFileSystem(nsHC::CFileSystemT::MakeFileSystem(Path));
                QVERIFY(Fs && Fs->IsRemote());
                QCOMPARE(Fs->GetRoot().toUpper(), Root.toUpper());
            }
            if (Synthetic)
            {
                QElapsedTimer ChangeTimer;
                ChangeTimer.start();
                Panel->ChangeVolume(Root);
                QVERIFY2(ChangeTimer.elapsed() < 300, "Selecting a disconnected drive blocked the GUI");
                QTRY_VERIFY_WITH_TIMEOUT(!model(Panel)->IsLoading(), 5000);
                QVERIFY(static_cast<CFSModel*>(model(Panel))->LastNavigationError().has_value());
                Panel->ChangeCurrentPath(unc_, {});
                QTRY_VERIFY_WITH_TIMEOUT(!model(Panel)->IsLoading(), 5000);
                QCOMPARE(model(Panel)->GetCurrentPathWithRoot(), unc_);
                Panel->RefreshVolumeList();
            }
            if (!Synthetic && qEnvironmentVariableIsSet("HC_TEST_MAPPED_IO"))
            {
                QTemporaryDir Corpus(Root + "/hc-startup-XXXXXX");
                QVERIFY(Corpus.isValid());
                qInfo() << "Mapped drive corpus:" << Corpus.path();
                const QByteArray Payload("mapped drive read/write\r\n");
                QFile Source(Corpus.filePath("startup.txt"));
                QVERIFY(Source.open(QIODevice::WriteOnly));
                QCOMPARE(Source.write(Payload), Payload.size());
                Source.close();
                for (bool Enabled : {false, true})
                {
                    auto* Model = static_cast<CFSModel*>(model(Panel));
                    Model->SetProviderEnabled(Enabled);
                    QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 5000);
                    Panel->ChangeCurrentPath(QDir::toNativeSeparators(Corpus.path()), {});
                    QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 15000);
                    QCOMPARE(names(Model), QStringList{"startup.txt"});
                    if (Enabled)
                    {
                        auto Reader = Mapped->ReadProvider();
                        auto Writer = hc::storage::MakeNativeWritableStorage(hc::storage::LocatorKind::Local, Reader);
                        const auto Destination = Mapped->ReadLocation(QDir::toNativeSeparators(Corpus.filePath("created")));
                        auto Created = QtConcurrent::run([Writer, Destination] { return Writer->MakeDirectory(Destination, {}); });
                        QTRY_VERIFY_WITH_TIMEOUT(Created.isFinished(), 15000);
                        QVERIFY2(Created.result().HasValue(), "Mapped-drive mutation must retain drive-letter locator semantics");
                        for (int Row = 0; Row < Model->rowCount(); ++Row)
                        {
                            const auto Entry = Model->GetFileInfo(Model->index(Row, 0));
                            if (Entry->Name_ != "startup.txt") continue;
                            QVERIFY(Entry->storageEntry_.has_value());
                            auto Opened = Reader->OpenRead(Entry->storageEntry_->id_, {});
                            QTRY_VERIFY_WITH_TIMEOUT(Opened.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 15000);
                            auto Stream = Opened.get(); QVERIFY(Stream.HasValue());
                            auto Read = Stream.Value()->Read(1024, {});
                            QTRY_VERIFY_WITH_TIMEOUT(Read.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 15000);
                            auto Chunk = Read.get(); QVERIFY(Chunk.HasValue());
                            QCOMPARE(QByteArray(reinterpret_cast<const char*>(Chunk.Value().bytes_.data()), Chunk.Value().bytes_.size()), Payload);
                        }
                    }
                    Panel->ChangeCurrentPath(unc_, {});
                    QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 5000);
                }
            }
        }
        if (Synthetic) QCOMPARE(DriveProbeFixture::MetadataCalls(), 0);
    }

    void shareNavigation_data()
    {
        QTest::addColumn<bool>("Enabled");
        QTest::newRow("legacy") << false;
        QTest::newRow("provider") << true;
    }
    void shareNavigation()
    {
        QFETCH(bool, Enabled);
        const auto Expected = QDir(unc_).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
        for (auto* Panel : panels_)
        {
            static_cast<CFSModel*>(model(Panel))->SetProviderEnabled(Enabled);
            QTRY_VERIFY_WITH_TIMEOUT(!model(Panel)->IsLoading(), 15000);
            Panel->ChangeCurrentPath(unc_, {});
            auto* Model = model(Panel);
            QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 15000);
            QCOMPARE(Model->GetCurrentPathWithRoot(), unc_);
            QCOMPARE(names(Model), Expected);
            QThreadPool::globalInstance()->waitForDone();
            Panel->RefreshSource(Panel->CurrentTabIndex());
            QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 15000);
            QCOMPARE(names(Model), Expected);
            QThreadPool::globalInstance()->waitForDone();
            Model->ChangeDirectory("..");
            QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 15000);
            QCOMPARE(Model->GetCurrentPathWithRoot(), "\\\\" + server_);
            QVERIFY(names(Model).contains(share_));
            QThreadPool::globalInstance()->waitForDone();
            Model->ChangeDirectory(share_);
            QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 15000);
            QCOMPARE(Model->GetCurrentPathWithRoot(), unc_);
            QCOMPARE(names(Model), Expected);
            QThreadPool::globalInstance()->waitForDone();
        }
    }

    void missingDirectoryAndRecovery()
    {
        auto* Panel = panels_.front();
        auto* Model = model(Panel);
        Panel->ChangeCurrentPath(unc_, {});
        QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading() && Model->GetCurrentPathWithRoot() == unc_, 15000);
        const auto Missing = unc_ + "\\__hc_missing_" + QUuid::createUuid().toString(QUuid::Id128);
        QVERIFY(!QDir(Missing).exists());
        QElapsedTimer Timer;
        Timer.start();
        Panel->ChangeCurrentPath(Missing, {});
        qInfo() << "Missing-directory UI call milliseconds:" << Timer.elapsed();
        QVERIFY(Timer.elapsed() < 15000);
        QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 15000);
        const auto Failure = static_cast<CFSModel*>(Model)->LastNavigationError();
        QVERIFY(Failure.has_value());
        QCOMPARE(Failure->category_, hc::storage::ErrorCategory::NotFound);
        QCOMPARE(Model->GetCurrentPathWithRoot(), unc_);
        bool Tick = false;
        QTimer::singleShot(0, this, [&Tick] { Tick = true; });
        QTRY_VERIFY(Tick);
        Panel->ChangeCurrentPath(unc_, {});
        QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 15000);
        QCOMPARE(Model->GetCurrentPathWithRoot(), unc_);
        QVERIFY(!names(Model).isEmpty());
        QThreadPool::globalInstance()->waitForDone();
        QVERIFY(main_->grab().save(QCoreApplication::applicationDirPath() + "/smb-panels.png"));
    }

    void cancelledAndLateNavigation()
    {
        auto* Panel = panels_.front();
        auto* Model = static_cast<CFSModel*>(model(Panel));
        Panel->ChangeCurrentPath(unc_, {});
        QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading() && Model->GetCurrentPathWithRoot() == unc_, 15000);
        for (bool Cancel : {true, false})
        {
            auto Provider = std::make_shared<GatedProvider>();
            auto Release = qScopeGuard([&] { Provider->release_.set_value(); });
            Model->SetRoot(nsHC::TySpFileSystem(new GatedFs(Provider)));
            Model->SetCurrentPath("\\");
            Model->ChangeDirectory("");
            QTRY_VERIFY(Provider->started_.load());
            QVERIFY(Model->IsLoading());
            bool Tick = false;
            QTimer::singleShot(0, this, [&] { Tick = true; });
            QTRY_VERIFY(Tick);
            if (Cancel) Model->CancelNavigation();
            else Panel->ChangeCurrentPath(unc_, {});
            QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 15000);
            QCOMPARE(Model->GetCurrentPathWithRoot(), unc_);
        }
        QThreadPool::globalInstance()->waitForDone();
        QCoreApplication::processEvents();
        QCOMPARE(Model->GetCurrentPathWithRoot(), unc_);
    }

    void emptyAndWcxNavigation()
    {
        auto* Panel = panels_.front();
        auto* Model = static_cast<CFSModel*>(model(Panel));
        QTemporaryDir Directory;
        QVERIFY(Directory.isValid());
        Panel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {});
        QTRY_VERIFY(!Model->IsLoading());
        QVERIFY(names(Model).isEmpty());
        const auto ArchivePath = Directory.filePath("fixture.hcwcx");
        QFile Archive(ArchivePath);
        QVERIFY(Archive.open(QIODevice::WriteOnly));
        Archive.write("HCWCX01");
        Archive.close();
        TyPlugInWCX Spec;
        Spec.FileFullPath = QCoreApplication::applicationDirPath() + "/hc_wcx_fixture.wcx64";
        auto Plugin = TySpWCX(new CTCPlugInWCX);
        QCOMPARE(Plugin->LoadWCX(Spec).ErrorCode, 0u);
        QVERIFY(Plugin->OpenArchive(ArchivePath).Value.value_or(false));
        auto Pack = nsHC::TySpFileSystem(new nsHC::CFSPack(ArchivePath, Plugin));
        for (bool Enabled : {false, true})
        {
            Model->SetProviderEnabled(Enabled);
            QTRY_VERIFY(!Model->IsLoading());
            Panel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {});
            QTRY_VERIFY(!Model->IsLoading());
            Model->AppendRoot(Pack);
            Model->ChangeDirectory("/");
            QTRY_VERIFY(!Model->IsLoading());
            QCOMPARE(names(Model), QStringList({"alpha.txt", "sub"}));
            Model->ChangeDirectory("sub");
            QTRY_VERIFY(!Model->IsLoading());
            QCOMPARE(names(Model), QStringList({QString::fromUtf8("한글.txt")}));
            if (Enabled)
            {
                auto* Bridge = TyStCommandMgr::GetInstance()->Bridge();
                const auto Destination = Directory.filePath("extracted"); QVERIFY(QDir().mkdir(Destination));
                QModelIndex Selected;
                for (int Row = 0; Row < Model->rowCount(); ++Row)
                    if (Model->GetName(Model->index(Row, 0)) == QString::fromUtf8("한글.txt")) Selected = Model->index(Row, 0);
                QVERIFY(Selected.isValid());
                auto Copy = Bridge->Dispatch("hc.file.copy", {{"destination", Destination}}, "test", Selected);
                QTRY_VERIFY(Copy.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
                auto Accepted = Copy.get(); QVERIFY(Accepted.HasValue());
                auto& Scheduler = Bridge->FileJobs()->Scheduler();
                QTRY_VERIFY(hc::operations::IsTerminal(Scheduler.Snapshot(Accepted.Value().jobId_).Value().state_));
                QCOMPARE(Scheduler.Snapshot(Accepted.Value().jobId_).Value().state_, hc::operations::JobState::Completed);
                QFile Copied(Destination + "/한글.txt"); QVERIFY(Copied.open(QIODevice::ReadOnly)); QCOMPARE(Copied.readAll(), QByteArray("한글!\r\n"));
                QTest::qWait(150); QTRY_VERIFY(!Model->IsLoading());
                QCOMPARE(names(Model), QStringList({QString::fromUtf8("한글.txt")}));
            }
            Model->ChangeDirectory("..");
            QTRY_VERIFY(!Model->IsLoading());
            QCOMPARE(names(Model), QStringList({"alpha.txt", "sub"}));
            QThreadPool::globalInstance()->waitForDone();
        }
        Panel->ChangeCurrentPath(unc_, {});
        QTRY_VERIFY(!Model->IsLoading());
    }

    void rhwpDownloadsTimingUi() {
        const auto Root=qEnvironmentVariable("HC_RHWP_TIMING_ROOT");if(Root.isEmpty())QSKIP("Explicit original-file timing run only");
        auto& Settings=hc::settings::AppSettings();const auto Saved=Settings.Object();const auto Workspace=main_->CaptureWorkspace();
        const auto Restore=qScopeGuard([&]{Settings.Replace(Saved);TyStCommandMgr::GetInstance()->Refresh();main_->RestoreWorkspace(Workspace);});
        QVERIFY(Settings.Put(hc::settings::Section::Editors,QJsonArray{}));QVERIFY(Settings.SetPlugins("wlx",{}));QVERIFY(Settings.SetPlugins("dopusViewer",{}));
        auto* Source=panels_.front();auto* Model=static_cast<CFSModel*>(model(Source));Model->SetProviderEnabled(true);
        Source->ChangeCurrentPath(QDir::toNativeSeparators(Root),{});QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(),15000);
        auto* Grid=qobject_cast<CGridView*>(Source->GetFocusView(Source->CurrentTabIndex()));QVERIFY(Grid);
        auto* Controller=main_->findChild<QObject*>("hc.document.openController");QVERIFY(Controller);
        const auto Select=[&](const QString& Name)->QWidget* {
            bool Found=false;
            for(int Row=0;Row<Grid->BaseView()->modelController()->getGridRowCount();++Row){
                Grid->BaseView()->setFocusedRowIndex(Row);auto Index=Grid->BaseView()->focusedRow().modelIndex(0);
                while(const auto Proxy=qobject_cast<const QAbstractProxyModel*>(Index.model()))Index=Proxy->mapToSource(Index);
                if(Model->GetName(Index)==Name){Found=true;break;}
            }
            if(!Found)return nullptr;main_->activateWindow();QApplication::setActiveWindow(main_);Grid->EnsureKeyboardFocusOnView();return QApplication::focusWidget();
        };
        QVERIFY(hc::test::RunRhwpOpenTiming(Controller,Select));
    }
    void rhwpF3ProductUi() { rhwpProductKeyUi(false); }
    void rhwpF4ProductUi() { rhwpProductKeyUi(true); }
private:
    void rhwpProductKeyUi(bool Editable) {
        const auto Corpus = qEnvironmentVariable("HC_RHWP_TEST_SDK_CORPUS");
        if (Corpus.isEmpty()) QSKIP("Pinned RHWP product corpus not requested");
        auto& Settings = hc::settings::AppSettings(); const auto Saved = Settings.Object(); const auto Workspace = main_->CaptureWorkspace();
        const auto Restore = qScopeGuard([&] { Settings.Replace(Saved); TyStCommandMgr::GetInstance()->Refresh(); main_->RestoreWorkspace(Workspace); });
        QVERIFY(Settings.Put(hc::settings::Section::Editors, QJsonArray{})); QVERIFY(Settings.SetPlugins("wlx", {})); QVERIFY(Settings.SetPlugins("dopusViewer", {}));
        QTemporaryDir Directory;
        for (const auto& Name : {QString("c02.hwp"), QString("c05.hwpx")}) QVERIFY(QFile::copy(QDir(Corpus).filePath(Name), Directory.filePath(Name)));
        auto* Source = panels_.front(); auto* Model = static_cast<CFSModel*>(model(Source)); Model->SetProviderEnabled(true);
        Source->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {}); QTRY_VERIFY(!Model->IsLoading());
        auto* Grid = qobject_cast<CGridView*>(Source->GetFocusView(Source->CurrentTabIndex())); QVERIFY(Grid);
        for (const auto& Name : {QString("c02.hwp"), QString("c05.hwpx")}) {
            bool Selected = false;
            for (int Row = 0; Row < Grid->BaseView()->modelController()->getGridRowCount(); ++Row) {
                Grid->BaseView()->setFocusedRowIndex(Row); auto Index = Grid->BaseView()->focusedRow().modelIndex(0);
                while (const auto Proxy = qobject_cast<const QAbstractProxyModel*>(Index.model())) Index = Proxy->mapToSource(Index);
                if (Model->GetName(Index) == Name) { Selected = true; break; }
            }
            QVERIFY(Selected); main_->activateWindow(); QApplication::setActiveWindow(main_); Grid->EnsureKeyboardFocusOnView();
            QVERIFY(QApplication::focusWidget()); QTest::keyClick(QApplication::focusWidget(), (Editable ? Qt::Key_F4 : Qt::Key_F3));
            auto* Controller = main_->findChild<QObject*>("hc.document.openController"); QVERIFY(Controller);
            QTRY_VERIFY_WITH_TIMEOUT(Controller->findChild<QProcess*>(), 15000);
            QPointer<QProcess> Viewer = Controller->findChild<QProcess*>();
            const auto Cleanup = qScopeGuard([&] { if (Viewer && Viewer->state() != QProcess::NotRunning) { Viewer->kill(); Viewer->waitForFinished(); } });
            QTRY_COMPARE(Viewer->state(), QProcess::Running);
            QCOMPARE(Viewer->arguments(), (QStringList{Editable ? "--rhwp-edit" : "--rhwp-view", "--", QDir::toNativeSeparators(Directory.filePath(Name))}));
            struct Search { DWORD pid_; HWND window_ = nullptr; } State{DWORD(Viewer->processId())};
            const auto Find = [&] {
                EnumWindows([](HWND Window, LPARAM Param) -> BOOL {
                    auto& State = *reinterpret_cast<Search*>(Param); DWORD Pid = 0; GetWindowThreadProcessId(Window, &Pid);
                    if (Pid == State.pid_ && IsWindowVisible(Window) && GetWindowTextLengthW(Window) > 0) { State.window_ = Window; return FALSE; }
                    return TRUE;
                }, reinterpret_cast<LPARAM>(&State)); return State.window_ != nullptr;
            };
            QTRY_VERIFY_WITH_TIMEOUT(Find(), 10000);
            const auto ReadyTitle = Name + (Editable ? QStringLiteral(" — 한글 문서 편집") : QStringLiteral(" — 한글 문서 보기"));
            const auto Title = [&] { wchar_t Text[1024]{}; GetWindowTextW(State.window_, Text, 1024); return QString::fromWCharArray(Text); };
            // The title leaves loading only after native parse + first-frame admission.
            // A surviving failure window is not a successful product open.
            QTRY_VERIFY_WITH_TIMEOUT(Title() == ReadyTitle || Title().endsWith(QStringLiteral(" — 열기 실패")), 30000);
            QCOMPARE(Title(), ReadyTitle);
            QVERIFY(Viewer && Viewer->state() == QProcess::Running);
            PostMessageW(State.window_, WM_CLOSE, 0, 0); QTRY_VERIFY_WITH_TIMEOUT(Viewer.isNull(), 15000);
        }
    }
private slots:
    void epubProductKeys_data() {
        QTest::addColumn<bool>("Editor"); QTest::addColumn<QString>("Book");
        for(bool Editor : {false, true}) for(const auto* Book : {"epub2-ncx.epub", "epub3-images.epub"})
            QTest::newRow(qPrintable(QString(Editor ? "F4-" : "F3-") + Book)) << Editor << QString(Book);
        const auto StartupBook=qEnvironmentVariable("HC_EPUB_STARTUP_BOOK");
        if(!StartupBook.isEmpty())QTest::newRow("F3-startup-book")<<false<<StartupBook;
    }
    void epubProductKeys() {
        QFETCH(bool, Editor); QFETCH(QString, Book);
        const auto Corpus = qEnvironmentVariable("HC_EPUB_CORPUS"); if(Corpus.isEmpty()) QSKIP("Explicit EPUB product corpus required");
        auto& Settings = hc::settings::AppSettings(); const auto Saved = Settings.Object(); const auto Workspace = main_->CaptureWorkspace();
        auto Restore = qScopeGuard([&] { Settings.Replace(Saved); TyStCommandMgr::GetInstance()->Refresh(); main_->RestoreWorkspace(Workspace); });
        QVERIFY(Settings.Put(hc::settings::Section::Editors, QJsonArray{})); QVERIFY(Settings.SetPlugins("wlx", {})); QVERIFY(Settings.SetPlugins("dopusViewer", {}));
        const auto Name=QFileInfo(Book).fileName();
        QTemporaryDir Directory; const auto Path = Directory.filePath(Name); QVERIFY(QFile::copy(QFileInfo(Book).isAbsolute()?Book:QDir(Corpus).filePath(Book), Path));
        QFile Original(Path); QVERIFY(Original.open(QIODevice::ReadOnly)); const auto Bytes = Original.readAll(); Original.close();
        auto* Source = panels_.front(); auto* Model = static_cast<CFSModel*>(model(Source)); Model->SetProviderEnabled(true);
        Source->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {}); QTRY_VERIFY(!Model->IsLoading());
        auto* Grid = qobject_cast<CGridView*>(Source->GetFocusView(Source->CurrentTabIndex())); QVERIFY(Grid);
        bool Selected = false;
        for(int Row=0; Row<Grid->BaseView()->modelController()->getGridRowCount(); ++Row) {
            Grid->BaseView()->setFocusedRowIndex(Row); auto Index = Grid->BaseView()->focusedRow().modelIndex(0);
            while(const auto Proxy = qobject_cast<const QAbstractProxyModel*>(Index.model())) Index = Proxy->mapToSource(Index);
            if(Model->GetName(Index)==Name) { Selected=true; break; }
        }
        QVERIFY(Selected); main_->activateWindow(); QApplication::setActiveWindow(main_); Grid->EnsureKeyboardFocusOnView(); QVERIFY(QApplication::focusWidget());
        struct LoadingWindowProbe final : QObject {
            bool shown=false;
            bool eventFilter(QObject* Object,QEvent* Event) override {
                if(Event->type()==QEvent::Show && Object->objectName()=="hc.document.workspace")shown=true;
                return false;
            }
        } LoadingProbe;
        qApp->installEventFilter(&LoadingProbe);
        qInfo("EPUB product key dispatch");
        QElapsedTimer Startup;Startup.start();
        QTest::keyClick(QApplication::focusWidget(), Editor ? Qt::Key_F4 : Qt::Key_F3);
        qInfo("EPUB product key dispatched");
        auto* Controller = main_->findChild<QObject*>("hc.document.openController"); QVERIFY(Controller);
        if(!Editor) {
            QTRY_VERIFY_WITH_TIMEOUT(main_->findChild<QDialog*>("hc.document.workspace"),15000);
            QPointer<QDialog> Window = main_->findChild<QDialog*>("hc.document.workspace");
            const bool LoadingWindowShown=Window->isVisible();
            auto Cleanup=qScopeGuard([&] { if(Window && Window->objectName()=="hc.document.workspace") Window->close(); });
            auto* Surface=Window->findChild<QtPluginViewerSurface*>(); QVERIFY(Surface);
            HWND PresentedWindow=nullptr;bool Disappeared=false;qint64 PresentedMs=-1;
            QTimer Visibility;Visibility.setInterval(5);
            connect(&Visibility,&QTimer::timeout,&Visibility,[&]{
                if(PresentedWindow){Disappeared|=!IsWindowVisible(PresentedWindow);return;}
                struct Search {DWORD pid;HWND window=nullptr;} Candidate{DWORD(Surface->Details().host_.processId_)};
                if(!Candidate.pid)return;
                EnumWindows([](HWND Window,LPARAM Value)->BOOL {
                    auto& State=*reinterpret_cast<Search*>(Value);DWORD Pid=0;GetWindowThreadProcessId(Window,&Pid);
                    if(Pid==State.pid && IsWindowVisible(Window) && GetWindowTextLengthW(Window)>0){State.window=Window;return FALSE;}return TRUE;
                },reinterpret_cast<LPARAM>(&Candidate));
                if(Candidate.window){PresentedWindow=Candidate.window;PresentedMs=Startup.elapsed();}
            });Visibility.start();
            QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady() || Surface->Snapshot().status_ == hc::viewer::ViewerStatus::Failed,60000);
            QVERIFY2(Surface->IsReady(), Surface->Snapshot().error_ ? Surface->Snapshot().error_->code_.c_str() : "viewer not ready");
            QTRY_VERIFY_WITH_TIMEOUT(Surface->Details().documentState_["documents"].toArray().first().toObject()["view"].toObject()["ready"].toBool()
                || Surface->Snapshot().status_ == hc::viewer::ViewerStatus::Failed,60000);
            QVERIFY(Surface->Details().documentState_["documents"].toArray().first().toObject()["view"].toObject()["ready"].toBool());
            const auto Document=Surface->Details().documentState_["documents"].toArray().first().toObject();
            QCOMPARE(Document["view"].toObject()["viewId"].toString(),QString("epub"));
            QVERIFY(Document["view"].toObject()["readOnly"].toBool()); QVERIFY(!Document["canSave"].toBool());
            const auto Reader = Document["view"].toObject()["epub"].toObject();
            qInfo().noquote()<<"EPUB startup"<<QJsonDocument(QJsonObject{{"elapsedMs",Startup.elapsed()},{"firstWindowMs",PresentedMs},{"loadingWindowShown",LoadingWindowShown||LoadingProbe.shown},{"nativeWindowDisappeared",Disappeared},{"metrics",Reader["metrics"]},{"startup",Reader["startup"]}}).toJson(QJsonDocument::Compact);
            QVERIFY(!LoadingWindowShown);QVERIFY(!LoadingProbe.shown);QVERIFY(!Disappeared);
            QVERIFY(!Reader["text"].toString().trimmed().isEmpty() || !Reader["images"].toArray().isEmpty());
            for(const auto& Value : Reader["images"].toArray()) { const auto Image = Value.toObject(); QVERIFY(Image["complete"].toBool()); QVERIFY(Image["naturalWidth"].toInt() > 0); }
            struct Search { DWORD pid; HWND window=nullptr; } State{DWORD(Surface->Details().host_.processId_)};
            const auto Find=[&] {
                EnumWindows([](HWND Candidate,LPARAM Value)->BOOL {
                    auto& Item=*reinterpret_cast<Search*>(Value); DWORD Pid=0; GetWindowThreadProcessId(Candidate,&Pid);
                    if(Pid==Item.pid && IsWindowVisible(Candidate) && GetWindowTextLengthW(Candidate)>0) { Item.window=Candidate; return FALSE; } return TRUE;
                },reinterpret_cast<LPARAM>(&State)); return State.window!=nullptr;
            };
            QTRY_VERIFY_WITH_TIMEOUT(Find(),10000);
            Visibility.stop();QVERIFY(!Disappeared);
            PostMessageW(State.window,WM_CLOSE,0,0);
            QTRY_VERIFY_WITH_TIMEOUT(Window.isNull() || (!Window->isVisible() && Window->objectName()=="hc.document.prepared"),20000);
            QTRY_VERIFY_WITH_TIMEOUT(Window.isNull() || Surface->Details().host_.activeProcesses_<=1u,15000);
        } else {
            QTRY_VERIFY_WITH_TIMEOUT(Controller->findChild<QProcess*>(),15000);
            QPointer<QProcess> Process=Controller->findChild<QProcess*>();
            QTRY_COMPARE_WITH_TIMEOUT(Process->state(), QProcess::Running, 15000);
            auto Cleanup=qScopeGuard([&] { if(Process && Process->state()!=QProcess::NotRunning) { Process->kill(); Process->waitForFinished(); } });
            QVERIFY(Process->arguments().contains("--document-appearance"));
            struct Search { DWORD pid; HWND window=nullptr; } State{DWORD(Process->processId())};
            const auto Find=[&] {
                EnumWindows([](HWND Window,LPARAM Value)->BOOL {
                    auto& Item=*reinterpret_cast<Search*>(Value); DWORD Pid=0; GetWindowThreadProcessId(Window,&Pid);
                    if(Pid==Item.pid && IsWindowVisible(Window) && GetWindowTextLengthW(Window)>0) { Item.window=Window; return FALSE; } return TRUE;
                },reinterpret_cast<LPARAM>(&State)); return State.window!=nullptr;
            };
            QTRY_VERIFY_WITH_TIMEOUT(Find(),15000);
            const auto Title=[&] { wchar_t Text[1024]{}; GetWindowTextW(State.window,Text,1024); return QString::fromWCharArray(Text); };
            QTRY_VERIFY_WITH_TIMEOUT(Title().contains(QStringLiteral("읽기 전용")) || Title().contains(QStringLiteral("열기 실패")),60000);
            QVERIFY2(Title().contains(QStringLiteral("읽기 전용")),qPrintable(Title()));
            PostMessageW(State.window,WM_CLOSE,0,0); QTRY_VERIFY_WITH_TIMEOUT(Process.isNull(),20000);
        }
        QVERIFY(Original.open(QIODevice::ReadOnly)); QCOMPARE(Original.readAll(),Bytes);
    }
    void epubStartupFailureUi() {
        const auto Catalog=hc::document::BuildDocumentHandlerCatalog(hc::settings::AppSettings(),QCoreApplication::applicationDirPath());
        const auto Found=Catalog.targets_.find("builtin.hyperviewer");QVERIFY(Found!=Catalog.targets_.end());QVERIFY(Found->second.descriptor_.available_);
        QTemporaryDir Directory;QFile File(Directory.filePath("invalid.epub"));QVERIFY(File.open(QIODevice::WriteOnly));File.write("invalid epub");File.close();
        auto Provider=hc::storage::MakeNativeReadProvider(hc::storage::LocatorKind::Local);
        const auto Entry=Provider->Resolve({hc::storage::LocatorKind::Local,File.fileName().toStdString()},{}).get();QVERIFY(Entry.HasValue());
        auto Source=std::make_shared<hc::storage::ProviderContentSource>(Provider,Entry.Value());
        QtViewerHost Host(main_,[]{return hc::storage::MakeNativeMaterializer(QDir::tempPath().toStdString());});
        auto Drain=qScopeGuard([&]{Host.CloseAll();while(!Host.IsIdle())QTest::qWait(10);});
        bool Completed=false,Ready=true;QString Error;
        QPointer<QDialog> Window=Host.OpenPlugin(Found->second.plugin_,{{Source,{}}},"EPUB failure fixture",[&](bool Ok,const QString& Code){Completed=true;Ready=Ok;Error=Code;});
        QVERIFY(Window);QVERIFY(!Window->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(Completed,30000);QVERIFY(!Ready);QVERIFY(!Error.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(Window && Window->isVisible(),10000);QVERIFY(Window->windowTitle().contains(Error));
        Window->close();QTRY_VERIFY_WITH_TIMEOUT(Window.isNull(),15000);
    }
    void documentCommandsUi() {
        auto& Settings = hc::settings::AppSettings(); const auto Saved = Settings.Object(); const auto Workspace = main_->CaptureWorkspace();
        const auto Restore = qScopeGuard([&] { Settings.Replace(Saved); TyStCommandMgr::GetInstance()->Refresh(); main_->RestoreWorkspace(Workspace); });
        QVERIFY(Settings.Put(hc::settings::Section::Editors, QJsonArray{})); QVERIFY(Settings.SetPlugins("wlx", {})); QVERIFY(Settings.SetPlugins("dopusViewer", {}));
        QTemporaryDir Directory; const auto Path = Directory.filePath("한글 문서.go"); QFile File(Path); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("P4 document open"); File.close();
        auto* Source = panels_.front(); auto* Model = static_cast<CFSModel*>(model(Source)); Model->SetProviderEnabled(true);
        Source->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {}); QTRY_VERIFY(!Model->IsLoading());
        auto* Grid = qobject_cast<CGridView*>(Source->GetFocusView(Source->CurrentTabIndex())); QVERIFY(Grid);
        QModelIndex Selected;
        for (int Row = 0; Row < Grid->BaseView()->modelController()->getGridRowCount(); ++Row) {
            Grid->BaseView()->setFocusedRowIndex(Row); auto Index = Grid->BaseView()->focusedRow().modelIndex(0);
            while (const auto Proxy = qobject_cast<const QAbstractProxyModel*>(Index.model())) Index = Proxy->mapToSource(Index);
            if (Model->GetName(Index) == "한글 문서.go") { Selected = Index; break; }
        }
        QVERIFY(Selected.isValid()); Grid->EnsureKeyboardFocusOnView(); QVERIFY(QApplication::focusWidget());
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_F3);
        QTRY_VERIFY_WITH_TIMEOUT(main_->findChild<QDialog*>("hc.document.workspace"), 15000);
        QPointer<QDialog> Full = main_->findChild<QDialog*>("hc.document.workspace"); QCOMPARE(Full->windowTitle(), QString("HyperViewer"));
        auto* Surface = Full->findChild<QtPluginViewerSurface*>(); QVERIFY(Surface); QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(Surface->Details().documentState_["documents"].toArray().size(), 1, 15000);
        QVERIFY(!Surface->Details().documentState_["documents"].toArray()[0].toObject()["sourceToken"].toString().isEmpty());
        QTRY_VERIFY(!Surface->Details().documentState_["documents"].toArray()[0].toObject()["loading"].toBool());
        QVERIFY(Surface->Details().documentState_["documents"].toArray()[0].toObject()["error"].toString().isEmpty());
        const auto FocusedRow = Grid->BaseView()->focusedRowIndex();
        QVERIFY(sendWlxKey(Full, VK_ESCAPE, false));
        QTRY_VERIFY_WITH_TIMEOUT(Full.isNull(), 10000);
        QTRY_COMPARE(GetForegroundWindow(), reinterpret_cast<HWND>(main_->winId()));
        QTRY_VERIFY(QApplication::focusWidget() && (QApplication::focusWidget() == Grid || Grid->isAncestorOf(QApplication::focusWidget())));
        QCOMPARE(Grid->BaseView()->focusedRowIndex(), FocusedRow);
        QVERIFY(FocusedRow > 0);
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Up);
        QTRY_COMPARE(Grid->BaseView()->focusedRowIndex(), FocusedRow - 1);
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Down);
        QTRY_COMPARE(Grid->BaseView()->focusedRowIndex(), FocusedRow);
        auto* Host = static_cast<QtViewerHost*>(main_->findChild<QObject*>("hc.viewer.host"));
        // The hidden prepared viewer intentionally survives a document close.
        // Drain it explicitly before asserting that all isolated jobs stopped.
        Host->CloseAll(); QTRY_VERIFY_WITH_TIMEOUT(Host->IsIdle(), 15000);
        // No test-side activation/focus repair: F4 must work after ESC returns.
        QTRY_VERIFY(QApplication::focusWidget()); QTest::keyClick(QApplication::focusWidget(), Qt::Key_F4);
        auto* Controller = main_->findChild<QObject*>("hc.document.openController"); QVERIFY(Controller);
        QTRY_VERIFY_WITH_TIMEOUT(Controller->findChild<QProcess*>(), 15000); QPointer<QProcess> Editor = Controller->findChild<QProcess*>();
        const auto Cleanup = qScopeGuard([&] { if (Editor && Editor->state() != QProcess::NotRunning) { Editor->kill(); Editor->waitForFinished(); } });
        QTRY_COMPARE(Editor->state(), QProcess::Running); QVERIFY(Editor->program().endsWith("/HyperEditor.exe")); QCOMPARE(Editor->arguments().back(), QDir::toNativeSeparators(Path));
        Editor->kill(); Editor->waitForFinished(); QTRY_VERIFY(Editor.isNull());
        QVERIFY(Settings.Put(hc::settings::Section::Editors, QJsonArray{QJsonObject{{"name", "별도 편집기"}, {"program", QCoreApplication::applicationDirPath() + "/HyperEditor.exe"}, {"extensions", QJsonArray{"*"}}}}));
        QTimer::singleShot(100, [] { if (auto* Popup = qobject_cast<QMenu*>(QApplication::activePopupWidget())) Popup->close(); });
        Grid->EnsureKeyboardFocusOnView(); QTest::keyClick(QApplication::focusWidget(), Qt::Key_F4); QTest::qWait(300); QVERIFY(Controller->findChildren<QProcess*>().empty());
        Model->SetProviderEnabled(false); QTRY_VERIFY(!Model->IsLoading());
        const auto Unsupported = TyStCommandMgr::GetInstance()->Bridge()->Dispatch("hc.viewer.open", {}, "test", Selected).get();
        QVERIFY(!Unsupported.HasValue()); QCOMPARE(Unsupported.Failure().code_, std::string("document-provider-unsupported"));
        QVERIFY(main_->statusBar()->currentMessage().contains("document-provider-unsupported"));
        Model->SetProviderEnabled(true); QTRY_VERIFY(!Model->IsLoading());
    }
    void documentProductSavesUi() {
        auto& Settings = hc::settings::AppSettings(); const auto Saved = Settings.Object(); const auto Workspace = main_->CaptureWorkspace();
        const auto Restore = qScopeGuard([&] { Settings.Replace(Saved); TyStCommandMgr::GetInstance()->Refresh(); main_->RestoreWorkspace(Workspace); });
        QVERIFY(Settings.Put(hc::settings::Section::Editors, QJsonArray{})); QVERIFY(Settings.SetPlugins("wlx", {})); QVERIFY(Settings.SetPlugins("dopusViewer", {}));
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        const auto TextPath = Directory.filePath("한글.txt"), PdfPath = Directory.filePath("document.pdf"), ImagePath = Directory.filePath("alpha.png");
        { QFile File(TextPath); QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write("original\r\n"), 10); }
        { QPdfWriter Writer(PdfPath); QPainter Painter(&Writer); QVERIFY(Painter.isActive()); Painter.drawText(100, 300, "HC installed PDF"); Painter.end(); }
        { QImage Image(32, 24, QImage::Format_RGBA8888); Image.fill(QColor(70, 130, 190, 160)); QVERIFY(Image.save(ImagePath)); }
        const auto Read = [](const QString& Path) { QFile File(Path); return File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray{}; };
        auto* Source = panels_.front(); auto* Model = static_cast<CFSModel*>(model(Source)); Model->SetProviderEnabled(true);
        Source->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {}); QTRY_VERIFY(!Model->IsLoading());
        auto* Grid = qobject_cast<CGridView*>(Source->GetFocusView(Source->CurrentTabIndex())); QVERIFY(Grid);
        for (const auto& Path : {TextPath, PdfPath, ImagePath}) {
            bool Selected = false;
            for (int Row = 0; Row < Grid->BaseView()->modelController()->getGridRowCount(); ++Row) {
                Grid->BaseView()->setFocusedRowIndex(Row); auto Index = Grid->BaseView()->focusedRow().modelIndex(0);
                while (const auto Proxy = qobject_cast<const QAbstractProxyModel*>(Index.model())) Index = Proxy->mapToSource(Index);
                if (Model->GetName(Index) == QFileInfo(Path).fileName()) { Selected = true; break; }
            }
            QVERIFY(Selected); Grid->EnsureKeyboardFocusOnView(); QVERIFY(QApplication::focusWidget()); QTest::keyClick(QApplication::focusWidget(), Qt::Key_F3);
            QTRY_VERIFY_WITH_TIMEOUT(main_->findChild<QDialog*>("hc.document.workspace"), 15000);
            QPointer<QDialog> Full = main_->findChild<QDialog*>("hc.document.workspace");
            const auto Close = qScopeGuard([&] { if (Full) delete Full.data(); });
            auto* Surface = Full->findChild<QtPluginViewerSurface*>(); QVERIFY(Surface);
            QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady() && Surface->Details().documentState_["documents"].toArray().size() == 1
                && Surface->Details().documentState_["idle"].toBool(), 15000);
            const auto State = [&] { return Surface->Details().documentState_["documents"].toArray()[0].toObject(); };
            QTRY_VERIFY_WITH_TIMEOUT(State()["lifecycle"].toString() == "ready" && State()["canSave"].toBool() && !State()["sourceToken"].toString().isEmpty(), 15000);
            QVERIFY2(State()["error"].toString().isEmpty(), qPrintable(QJsonDocument(State()).toJson(QJsonDocument::Compact)));
            const auto Id = State()["documentId"].toString(), View = State()["view"].toObject()["viewId"].toString();
            const auto Send = [&](QJsonObject Request) {
                Request["documentId"] = Id; const auto Result = Surface->SendDocument(Request); if (!Result.HasValue()) return false;
                QElapsedTimer Timer; Timer.start();
                while (Surface->Details().commandSequence_ < Result.Value()) { if (Timer.elapsed() > 15000) return false; QTest::qWait(20); }
                return !Surface->Details().commandError_;
            };
            const QString Edited = QStringLiteral("P5 HC 저장 확인\r\n");
            if (View == "text") QVERIFY(Send({{"action","replaceText"},{"text",Edited}}));
            else if (View == "pdf") QVERIFY(Send({{"action","pdf.rotate"},{"angle",90}}));
            else { QCOMPARE(View, QString("image")); QVERIFY(Send({{"action","image.crop"},{"x",1},{"y",1},{"width",12},{"height",8}})); }
            QTRY_VERIFY(State()["dirty"].toBool()); QVERIFY(Send({{"action","save"}}));
            QTRY_VERIFY_WITH_TIMEOUT(!State()["dirty"].toBool() && !State()["saving"].toBool(), 15000);
            QVERIFY2(State()["error"].toString().isEmpty(), qPrintable(State()["error"].toString()));
            if (View == "text") QCOMPARE(Read(Path), Edited.toUtf8());
            else if (View == "image") QCOMPARE(QImage(Path).size(), QSize(12, 8));
            else {
                FPDF_InitLibrary(); const auto Shutdown = qScopeGuard([] { FPDF_DestroyLibrary(); });
                const auto Bytes = Read(Path); auto Pdf = FPDF_LoadMemDocument64(Bytes.constData(), Bytes.size(), nullptr); QVERIFY(Pdf);
                const auto ClosePdf = qScopeGuard([&] { FPDF_CloseDocument(Pdf); }); QCOMPARE(FPDF_GetPageCount(Pdf), 1);
                auto Page = FPDF_LoadPage(Pdf, 0); QVERIFY(Page); const auto ClosePage = qScopeGuard([&] { FPDF_ClosePage(Page); }); QCOMPARE(FPDFPage_GetRotation(Page), 1);
            }
            qInfo() << "Installed F3 provider save verified:" << View;
            Full->close(); QTRY_VERIFY(Full.isNull());
            auto* Host = static_cast<QtViewerHost*>(main_->findChild<QObject*>("hc.viewer.host")); QVERIFY(Host); QTRY_VERIFY_WITH_TIMEOUT(Host->IsIdle(), 15000);
        }
    }
    void viewerCommandsAndPreviewUi()
    {
        const auto PrivateBytes = [] {
            PROCESS_MEMORY_COUNTERS_EX Counters{}; Counters.cb = sizeof(Counters);
            return GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&Counters), sizeof(Counters))
                ? static_cast<quint64>(Counters.PrivateUsage) : quint64{};
        };
        const auto BeforeBytes = PrivateBytes(); QVERIFY(BeforeBytes > 0);
        auto PeakBytes = BeforeBytes;
        QTimer MemorySampler; MemorySampler.setInterval(25);
        connect(&MemorySampler, &QTimer::timeout, this, [&] { PeakBytes = std::max(PeakBytes, PrivateBytes()); }); MemorySampler.start();
        const auto CorpusRoot = localOnly_ ? QDir::tempPath() : unc_;
        QTemporaryDir Directory(QDir(CorpusRoot).filePath("hc-viewer-XXXXXX")); QVERIFY(Directory.isValid());
        qInfo() << "Viewer corpus location:" << Directory.path() << "SMB:" << !localOnly_;
        const auto Put = [&](const QString& Name, const QByteArray& Bytes) { QFile File(Directory.filePath(Name)); return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size(); };
        QVERIFY(Put("first.txt", "first viewer content")); QVERIFY(Put("second.md", "# Second\n\nMarkdown preview")); QVERIFY(Put("archive.hcwcx", "HCWCX01"));
        QImage Picture(160, 90, QImage::Format_RGB32); Picture.fill(QColor(40, 100, 180));
        QVERIFY(Picture.save(Directory.filePath("picture.png")));
        auto* SourcePanel = panels_.front(); auto* TargetPanel = panels_.back();
        for (auto* Panel : panels_)
        {
            static_cast<CFSModel*>(model(Panel))->SetProviderEnabled(true);
            Panel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {});
            QTRY_VERIFY_WITH_TIMEOUT(!model(Panel)->IsLoading(), 5000);
        }
        auto* Model = static_cast<CFSModel*>(model(SourcePanel));
        auto* Grid = qobject_cast<CGridView*>(SourcePanel->GetFocusView(SourcePanel->CurrentTabIndex())); QVERIFY(Grid);
        const auto Focus = [&](const QString& Name) {
            for (int Row = 0; Row < Grid->BaseView()->modelController()->getGridRowCount(); ++Row)
            {
                Grid->BaseView()->setFocusedRowIndex(Row); auto Index = Grid->BaseView()->focusedRow().modelIndex(0);
                while (const auto Proxy = qobject_cast<const QAbstractProxyModel*>(Index.model())) Index = Proxy->mapToSource(Index);
                if (Index.model() == Model && Model->GetName(Index) == Name) { Grid->EnsureKeyboardFocusOnView(); return Index; }
            }
            return QModelIndex{};
        };
        auto* Bridge = TyStCommandMgr::GetInstance()->Bridge(); QVERIFY(Bridge);
        const auto Dispatch = [&](const QString& Id, QModelIndex Index = {}) {
            if (Id == "hc.viewer.open") return legacyOpen(Model, Index);
            auto Future = Bridge->Dispatch(Id, {}, "test", Index); QElapsedTimer Wait; Wait.start();
            while (Future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) { if (Wait.elapsed() > 5000) return false; QTest::qWait(10); }
            auto Result = Future.get(); if (!Result.HasValue()) qWarning() << QString::fromStdString(Result.Failure().code_); return Result.HasValue();
        };
        auto First = Focus("first.txt"); QVERIFY(First.isValid());
        QVERIFY(QApplication::focusWidget()); QVERIFY(legacyOpen(Model, First));
        QTRY_VERIFY_WITH_TIMEOUT(main_->findChild<QDialog*>("hc.viewer.full"), 5000);
        QPointer<QDialog> Full = main_->findChild<QDialog*>("hc.viewer.full");
        auto* FullSurface = Full->findChild<QtViewerSurface*>(); QVERIFY(FullSurface);
        QTRY_VERIFY_WITH_TIMEOUT(FullSurface->IsReady(), 5000);
        QCOMPARE(FullSurface->findChild<QPlainTextEdit*>()->toPlainText(), QString("first viewer content"));
        QTest::keyClick(Full.data(), Qt::Key_Escape); QTRY_VERIFY(Full.isNull());

        auto* Tabs = TargetPanel->findChild<QTabWidget*>("tabWidget"); QVERIFY(Tabs);
        const auto OriginalCount = Tabs->count(); QPointer<QWidget> FirstTab = Tabs->widget(0);
        TargetPanel->AddTab(); const auto PreviewIndex = TargetPanel->CurrentTabIndex();
        QTRY_VERIFY(!model(TargetPanel)->IsLoading());
        QPointer<QWidget> OriginalView = Tabs->widget(PreviewIndex);
        First = Focus("first.txt"); QVERIFY(First.isValid());
        auto* QuickAction = main_->findChild<QAction*>("hc.menu.hc.viewer.quick"); QVERIFY(QuickAction); QuickAction->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(qobject_cast<CQuickView*>(TargetPanel->GetFocusView(PreviewIndex)), 5000);
        QPointer<CQuickView> Quick = qobject_cast<CQuickView*>(TargetPanel->GetFocusView(PreviewIndex));
        auto* Surface = Quick->findChild<QtViewerSurface*>(); QVERIFY(Surface);
        QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(), 5000);
        QCOMPARE(Tabs->currentIndex(), PreviewIndex); QCOMPARE(Tabs->widget(0), FirstTab.data()); QCOMPARE(Tabs->count(), OriginalCount + 1);
        QCOMPARE(Surface->findChild<QPlainTextEdit*>()->toPlainText(), QString("first viewer content"));
        QVERIFY(Focus("second.md").isValid());
        QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady() && Surface->Snapshot().source_.name_ == "second.md", 5000);
        QVERIFY(Surface->findChild<QTextBrowser*>()->toPlainText().contains("Second"));
        for (int Repeat = 0; Repeat < 12; ++Repeat)
        {
            QVERIFY(Focus("first.txt").isValid()); QTest::qWait(15);
            QVERIFY(Focus("picture.png").isValid()); QTest::qWait(15);
            QVERIFY(Focus("second.md").isValid()); QTest::qWait(15);
        }
        QVERIFY(Focus("picture.png").isValid());
        QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady() && Surface->Snapshot().source_.name_ == "picture.png", 5000);
        QVERIFY(Surface->findChild<QScrollArea*>("hc.viewer.image")->isVisible());
        QVERIFY(Focus("second.md").isValid());
        QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady() && Surface->Snapshot().source_.name_ == "second.md", 5000);
        QVERIFY(main_->grab().save("viewer-product-quick.png"));
        Surface->findChild<QTextBrowser*>()->setFocus(); QTest::keyClick(QApplication::focusWidget(), Qt::Key_Q, Qt::ControlModifier);
        QTRY_VERIFY_WITH_TIMEOUT(Quick.isNull(), 5000); QCOMPARE(Tabs->widget(PreviewIndex), OriginalView.data()); QCOMPARE(Tabs->widget(0), FirstTab.data());

        // Archive content follows the same commands; no fake native pathname.
        const auto ArchivePath = Directory.filePath("archive.hcwcx");
        TyPlugInWCX Spec; Spec.FileFullPath = QCoreApplication::applicationDirPath() + "/hc_wcx_fixture.wcx64";
        auto Plugin = TySpWCX(new CTCPlugInWCX); QCOMPARE(Plugin->LoadWCX(Spec).ErrorCode, 0u); QVERIFY(Plugin->OpenArchive(ArchivePath).Value.value_or(false));
        Model->AppendRoot(nsHC::TySpFileSystem(new nsHC::CFSPack(ArchivePath, Plugin))); Model->ChangeDirectory("/"); QTRY_VERIFY(!Model->IsLoading());
        const auto Archived = Focus("alpha.txt"); QVERIFY(Archived.isValid()); QVERIFY(Dispatch("hc.viewer.open", Archived));
        Full = main_->findChild<QDialog*>("hc.viewer.full"); QVERIFY(Full); FullSurface = Full->findChild<QtViewerSurface*>();
        QTRY_VERIFY_WITH_TIMEOUT(FullSurface->IsReady(), 5000); QCOMPARE(FullSurface->findChild<QPlainTextEdit*>()->toPlainText(), QString("alpha")); Full->reject(); QTRY_VERIFY(Full.isNull());
        QVERIFY(Dispatch("hc.viewer.quick", Archived)); Quick = qobject_cast<CQuickView*>(TargetPanel->GetFocusView(PreviewIndex)); QVERIFY(Quick);
        Surface = Quick->findChild<QtViewerSurface*>(); QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(), 5000);
        QCOMPARE(Surface->findChild<QPlainTextEdit*>()->toPlainText(), QString("alpha"));
        TargetPanel->CloseTab(); QTRY_VERIFY(Quick.isNull()); QVERIFY(OriginalView.isNull()); QCOMPARE(Tabs->count(), OriginalCount); QCOMPARE(Tabs->widget(0), FirstTab.data());
        SourcePanel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {}); QTRY_VERIFY(!Model->IsLoading());
        auto* Host = static_cast<QtViewerHost*>(main_->findChild<QObject*>("hc.viewer.host")); QVERIFY(Host);
        QTRY_VERIFY_WITH_TIMEOUT(Host->IsIdle(), 5000);
        const auto AfterBytes = PrivateBytes(); PeakBytes = std::max(PeakBytes, AfterBytes); MemorySampler.stop();
        qInfo() << "Viewer product private bytes: before=" << BeforeBytes << "sampledPeak=" << PeakBytes << "afterIdle=" << AfterBytes;
    }

    void viewerLifetimeSoak()
    {
        const int Cycles = qEnvironmentVariableIntValue("HC_VIEWER_SOAK_CYCLES");
        if (!Cycles) QSKIP("Opt-in product lifetime measurement: set HC_VIEWER_SOAK_CYCLES");
        QVERIFY(Cycles >= 60 && Cycles <= 2000);
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        const auto Put = [&](const QString& Name, const QByteArray& Bytes) { QFile File(Directory.filePath(Name)); return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size(); };
        QVERIFY(Put("large.txt", QByteArray("Viewer lifetime UTF-8 한글 text line\n").repeated(8000)));
        QVERIFY(Put("document.md", QByteArray("# Viewer lifetime\n\n**Markdown** and `code`\n\n").repeated(300)));
        { QImage Picture(2048, 2048, QImage::Format_RGB32); Picture.fill(QColor(40, 100, 180)); QVERIFY(Picture.save(Directory.filePath("picture.png"))); }
        auto* Source = panels_.front(); auto* Target = panels_.back();
        for (auto* Panel : panels_)
        {
            static_cast<CFSModel*>(model(Panel))->SetProviderEnabled(true);
            Panel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {}); QTRY_VERIFY(!model(Panel)->IsLoading());
        }
        auto* Model = static_cast<CFSModel*>(model(Source));
        auto* Grid = qobject_cast<CGridView*>(Source->GetFocusView(Source->CurrentTabIndex())); QVERIFY(Grid);
        auto* Host = static_cast<QtViewerHost*>(main_->findChild<QObject*>("hc.viewer.host")); QVERIFY(Host);
        auto* Bridge = TyStCommandMgr::GetInstance()->Bridge(); QVERIFY(Bridge);
        const QStringList Names{"large.txt", "picture.png", "document.md"};
        const auto Focus = [&](const QString& Name) {
            for (int Row = 0; Row < Grid->BaseView()->modelController()->getGridRowCount(); ++Row)
            {
                Grid->BaseView()->setFocusedRowIndex(Row); auto Index = Grid->BaseView()->focusedRow().modelIndex(0);
                while (const auto Proxy = qobject_cast<const QAbstractProxyModel*>(Index.model())) Index = Proxy->mapToSource(Index);
                if (Index.model() == Model && Model->GetName(Index) == Name) { Grid->EnsureKeyboardFocusOnView(); return Index; }
            }
            return QModelIndex{};
        };
        const auto Dispatch = [&](const QString& Id, QModelIndex Index) {
            if (Id == "hc.viewer.open") return legacyOpen(Model, Index);
            auto Future = Bridge->Dispatch(Id, {}, "test", Index); QElapsedTimer Wait; Wait.start();
            while (Future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) { if (Wait.elapsed() > 5000) return false; QTest::qWait(10); }
            return Future.get().HasValue();
        };
        struct Sample { quint64 bytes_; DWORD handles_, gdi_, user_; int widgets_; };
        const auto Measure = [] {
            PROCESS_MEMORY_COUNTERS_EX Counters{}; Counters.cb = sizeof(Counters); DWORD Handles = 0;
            const bool Valid = GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&Counters), sizeof(Counters))
                && GetProcessHandleCount(GetCurrentProcess(), &Handles);
            return Sample{Valid ? static_cast<quint64>(Counters.PrivateUsage) : 0, Handles,
                GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS), GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS), static_cast<int>(QApplication::allWidgets().size())};
        };
        QFile Samples(QCoreApplication::applicationDirPath() + "/viewer-lifetime.jsonl"); QVERIFY(Samples.open(QIODevice::WriteOnly));
        QElapsedTimer Elapsed; Elapsed.start(); std::vector<Sample> Values;
        for (int Cycle = 0; Cycle < Cycles; ++Cycle)
        {
            for (const auto& Name : Names)
            {
                const auto Index = Focus(Name); QVERIFY(Index.isValid()); QVERIFY(Dispatch("hc.viewer.open", Index));
                QPointer<QDialog> Full = main_->findChild<QDialog*>("hc.viewer.full"); QVERIFY(Full);
                auto* Surface = Full->findChild<QtViewerSurface*>(); QVERIFY(Surface); QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(), 10000);
                QCOMPARE(QString::fromStdString(Surface->Snapshot().source_.name_), Name);
                Full->reject(); QTRY_VERIFY(Full.isNull());
            }
            QVERIFY(Dispatch("hc.viewer.quick", Focus(Names.front())));
            QPointer<CQuickView> Quick = qobject_cast<CQuickView*>(Target->GetFocusView(Target->CurrentTabIndex())); QVERIFY(Quick);
            auto* Surface = Quick->findChild<QtViewerSurface*>(); QVERIFY(Surface);
            for (const auto& Name : Names)
            {
                QVERIFY(Focus(Name).isValid());
                QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady() && QString::fromStdString(Surface->Snapshot().source_.name_) == Name, 10000);
            }
            QVERIFY(Dispatch("hc.viewer.quick", Focus(Names.back()))); QTRY_VERIFY(Quick.isNull());
            QTRY_VERIFY_WITH_TIMEOUT(Host->IsIdle(), 10000);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); QCoreApplication::processEvents();
            QVERIFY(main_->findChildren<QtViewerSurface*>().isEmpty());
            const auto Current = Measure(); QVERIFY(Current.bytes_ > 0); Values.push_back(Current);
            const QJsonObject Record{{"cycle", Cycle + 1}, {"elapsedMs", Elapsed.elapsed()}, {"privateBytes", static_cast<qint64>(Current.bytes_)},
                {"handles", static_cast<int>(Current.handles_)}, {"gdi", static_cast<int>(Current.gdi_)}, {"user", static_cast<int>(Current.user_)}, {"widgets", Current.widgets_}};
            const auto Line = QJsonDocument(Record).toJson(QJsonDocument::Compact) + "\r\n"; QCOMPARE(Samples.write(Line), Line.size()); QVERIFY(Samples.flush());
            if ((Cycle + 1) % 10 == 0) qInfo() << "Viewer lifetime cycle" << Cycle + 1 << "privateBytes" << Current.bytes_ << "handles" << Current.handles_;
        }
        // Compare two equally sized windows after warmup; do not trim heaps/caches to mask growth.
        const auto Median = [&](int Begin, auto Field) {
            std::vector<quint64> Window;
            for (int Index = Begin; Index < Begin + 20; ++Index) Window.push_back(Field(Values[Index]));
            std::sort(Window.begin(), Window.end()); return Window[Window.size() / 2];
        };
        const auto Early = Median(10, [](const Sample& Value) { return Value.bytes_; });
        const auto Late = Median(Cycles - 20, [](const Sample& Value) { return Value.bytes_; });
        qInfo() << "Viewer lifetime cycles" << Cycles << "milliseconds" << Elapsed.elapsed() << "warm median bytes" << Early << "final median bytes" << Late;
        QVERIFY2(Late <= Early + 16ULL * 1024 * 1024, "Post-warmup private-byte growth exceeded 16 MiB; inspect viewer-lifetime.jsonl");
        QVERIFY(Median(Cycles - 20, [](const Sample& Value) { return Value.handles_; }) <= Median(10, [](const Sample& Value) { return Value.handles_; }) + 8);
        QVERIFY(Median(Cycles - 20, [](const Sample& Value) { return Value.gdi_; }) <= Median(10, [](const Sample& Value) { return Value.gdi_; }) + 4);
        QVERIFY(Median(Cycles - 20, [](const Sample& Value) { return Value.user_; }) <= Median(10, [](const Sample& Value) { return Value.user_; }) + 4);
        QCOMPARE(Median(Cycles - 20, [](const Sample& Value) { return Value.widgets_; }), Median(10, [](const Sample& Value) { return Value.widgets_; }));
        for (auto* Panel : panels_) { Panel->ChangeCurrentPath(unc_, {}); QTRY_VERIFY(!model(Panel)->IsLoading()); }
    }
    void pluginApplicationSelectionUi()
    {
        auto& Settings = hc::settings::AppSettings(); const auto Original = Settings.Read(hc::settings::Section::Plugins);
        const auto Restore = qScopeGuard([&] { Settings.Put(hc::settings::Section::Plugins, Original); TyStPlugInMgr::GetInstance()->Refresh(); });
        QJsonObject Rule{{"version", 1}, {"enabled", true}, {"mode", "extensions"}, {"extensions", QJsonArray{"rar", "lha"}}};
        QJsonObject Plugin{{"path", "scope-test.wcx64"}, {"extensions", QJsonArray{"7z", "rar", "lha"}}, {"hostEnabled", true}, {"application", Rule}};
        Settings.SetPlugins("wcx", QJsonArray{Plugin}); TyStPlugInMgr::GetInstance()->Refresh();
        auto* Manager = TyStPlugInMgr::GetInstance();
        for (const auto& Choice : Manager->RetrieveWCXByExts("7z", "sample.7z")) QVERIFY(Choice.FileFullPath != "scope-test.wcx64");
        QCOMPARE(Manager->RetrieveWCXByExts("rar", "sample.rar").front().FileFullPath, QString("scope-test.wcx64"));
        QCOMPARE(Manager->RetrieveWCXByExts("lha", "sample.lha").front().FileFullPath, QString("scope-test.wcx64"));
        Rule["mode"] = "pattern"; Rule["expression"] = "backup-*.lha"; Plugin["application"] = Rule; Settings.SetPlugins("wcx", QJsonArray{Plugin}); Manager->Refresh();
        QVERIFY(Manager->RetrieveWCXByExts("lha", "other.lha").isEmpty()); QCOMPARE(Manager->RetrieveWCXByExts("lha", "backup-1.lha").front().FileFullPath, QString("scope-test.wcx64"));
        Settings.Put(hc::settings::Section::Plugins, Original); Manager->Refresh();
        auto& Theme = fm::style::ThemeManager::instance(); const auto PreviousScheme = Theme.scheme(); Theme.setScheme(fm::style::ThemeManager::Scheme::Dark);
        const auto RestoreTheme = qScopeGuard([&] { Theme.setScheme(PreviousScheme); });
        QMainOpts Dialog(main_); Dialog.show();
        Dialog.findChild<QStackedWidget*>("stkOpts")->setCurrentWidget(Dialog.findChild<QWidget*>("pgPluginManage"));
        auto* Categories = Dialog.findChild<QListWidget*>("lstOptCate");
        for (int Row = 0; Row < Categories->count(); ++Row) if (Categories->item(Row)->data(Qt::UserRole).toInt() == Dialog.findChild<QStackedWidget*>("stkOpts")->indexOf(Dialog.findChild<QWidget*>("pgPluginManage"))) Categories->setCurrentRow(Row);
        auto* Catalog = Dialog.findChild<QTreeWidget*>("pluginCatalog"); QVERIFY(Catalog); QTRY_VERIFY(Catalog->isVisible());
        auto* Roles = Dialog.findChild<QListWidget*>("pluginRoles"); Roles->setCurrentRow(2);
        QVERIFY(Dialog.findChild<QTabWidget*>("pluginInspectorTabs")->isVisible());
        const auto Capture = qEnvironmentVariable("HC_PLUGIN_PRODUCT_CAPTURE"); if (!Capture.isEmpty()) { QTest::qWait(100); QVERIFY(Dialog.grab().save(Capture)); }
        const auto Hold = qEnvironmentVariableIntValue("HC_PLUGIN_CAPTURE_HOLD_MS"); if (Hold > 0) QTest::qWait(qMin(Hold, 60000));
        Dialog.reject(); QCOMPARE(Settings.Read(hc::settings::Section::Plugins), Original);
    }
    void configuredHostNavigation()
    {
        using namespace hc::plugins; using namespace hc::storage;
        QTemporaryDir Directory; QVERIFY(Directory.isValid()); TyPlugInWCX Spec; QVERIFY(configureWcx(Spec, Directory, true));
        const auto ArchivePath = Directory.filePath("configured.hcwcx");
        { QFile File(ArchivePath); QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write("HCWCX01"), 7); }
        const auto Original = hc::settings::AppSettings().Read(hc::settings::Section::Plugins);
        const auto Restore = qScopeGuard([&] {
            hc::settings::AppSettings().Put(hc::settings::Section::Plugins, Original); TyStPlugInMgr::GetInstance()->Refresh();
        });
        hc::settings::AppSettings().SetPlugins("wcx", {}); SetPluginField(hc::settings::AppSettings(), "wcx", 0, "path", Spec.FileFullPath);
        SetPluginField(hc::settings::AppSettings(), "wcx", 0, "extensions", QJsonArray{"hcwcx"}); SetPluginField(hc::settings::AppSettings(), "wcx", 0, "hostEnabled", true);
        SetPluginField(hc::settings::AppSettings(), "wcx", 0, "hostManifest", Spec.hostManifest_);
        auto* Panel = panels_.front(); auto* Model = static_cast<CFSModel*>(model(Panel)); Model->SetProviderEnabled(true);
        for (bool Approved : {true, false})
        {
            SetPluginField(hc::settings::AppSettings(), "wcx", 0, "hostReviewed", hc::settings::JsonObject(Approved ? EncodeManifest(Spec.hostApproval_.reviewed_) : QByteArray{}));
            hc::settings::AppSettings().Commit(); TyStPlugInMgr::GetInstance()->Refresh();
            Panel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {}); QTRY_VERIFY(!Model->IsLoading());
            QModelIndex Selected;
            for (int Row = 0; Row < Model->rowCount(); ++Row) if (Model->GetName(Model->index(Row, 0)) == "configured.hcwcx") Selected = Model->index(Row, 0);
            QVERIFY(Selected.isValid()); Model->ChangeDirectory(Selected); QTRY_VERIFY(!Model->IsLoading());
            if (Approved) QCOMPARE(names(Model), QStringList({"alpha.txt", "sub"}));
            else
            {
                QVERIFY(Model->LastNavigationError().has_value()); QCOMPARE(Model->LastNavigationError()->category_, ErrorCategory::AccessDenied);
                QVERIFY(names(Model).contains("configured.hcwcx"));
            }
            const auto Native = QDir::toNativeSeparators(Spec.FileFullPath).toStdWString(); QVERIFY(!GetModuleHandleW(Native.c_str()));
        }
        {
            QMainOpts Options(main_); Options.show(); QCoreApplication::processEvents();
            auto* Categories = Options.findChild<QListWidget*>("lstOptCate"); QVERIFY(Categories);
            for (int Row = 0; Row < Categories->count(); ++Row) if (Categories->item(Row)->data(Qt::UserRole).toInt() == Options.findChild<QStackedWidget*>("stkOpts")->indexOf(Options.findChild<QWidget*>("pgPluginManage"))) Categories->setCurrentRow(Row); QCoreApplication::processEvents();
            auto* Table = Options.findChild<QTableWidget*>("wcxHostTable"); QVERIFY(Table); QCOMPARE(Table->rowCount(), 1);
            QVERIFY(Options.findChild<QTreeWidget*>("pluginCatalog")->isVisible()); QVERIFY(Options.grab().save(QCoreApplication::applicationDirPath() + "/wcx-product-options.png"));
        }
        Panel->ChangeCurrentPath(unc_, {}); QTRY_VERIFY(!Model->IsLoading());
    }
    void builtinNestedArchives_data()
    {
        QTest::addColumn<QString>("Outer"); QTest::addColumn<QString>("Inner");
        for (const auto* Outer : {"7z", "zip"}) for (const auto* Inner : {"plain.7z", "plain.zip", "defaults.alz", "defaults.egg", "test_read_format_rar5_compressed.rar", "inner.iso"})
            QTest::newRow(qPrintable(QString(Outer) + '-' + Inner)) << QString(Outer) << QString(Inner);
    }
    void builtinNestedArchives()
    {
        QFETCH(QString, Outer); QFETCH(QString, Inner); using namespace hc::plugins;
        // Put the actual outer archive on the live share when this suite uses SMB.
        QTemporaryDir Directory(QDir(unc_).filePath("hc-builtin-nested-XXXXXX")); QVERIFY(Directory.isValid());
        if (Inner == "inner.iso") {
            QFile File(Directory.filePath("payload.txt")); QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write("nested ISO"), 10); File.close();
            hc::iso::RewriteRequest Request; Request.profile_ = HC_ISO_UDF_BRIDGE; Request.temporaryBase_ = Directory.path();
            Request.sources_ = {{File.fileName().toStdString(), "payload.txt", false}};
            auto Prepared = hc::iso::PrepareRewrite(Request).get(); QVERIFY(Prepared.HasValue()); QVERIFY(QFile::copy(Prepared.Value()->Path(), Directory.filePath(Inner)));
        } else {
            const auto Corpus = qEnvironmentVariable(Inner.endsWith(".egg") || Inner.endsWith(".alz") ? "HC_UNEGG_CORPUS" : "HC_SEVENZIP_CORPUS");
            QVERIFY(!Corpus.isEmpty()); QVERIFY(QFile::copy(QDir(QFileInfo(Corpus).absolutePath()).filePath(Inner), Directory.filePath(Inner)));
        }
        const auto Name = "outer." + Outer; const auto Path = Directory.filePath(Name);
        ArchiveUpdate Create{Path.toStdString(), Directory.path().toStdString(), {Inner.toStdString()}}; Create.creation_ = ArchiveCreationOptions{1, false};
        auto Made = MakeApprovedWcxBackend(BuiltinSevenZipOptions())->Update(Create, {}); QVERIFY2(Made.HasValue(), Made.HasValue() ? "" : Made.Failure().code_.c_str());
        auto* Panel = panels_.front(); auto* Model = static_cast<CFSModel*>(model(Panel)); Model->SetProviderEnabled(true);
        const auto Restore = qScopeGuard([&] { Panel->ChangeCurrentPath(unc_, {}); QThreadPool::globalInstance()->waitForDone(); });
        const auto Index = [&](const QString& Wanted) { for (int Row = 0; Row < Model->rowCount(); ++Row) if (Model->GetName(Model->index(Row, 0)) == Wanted) return Model->index(Row, 0); return QModelIndex{}; };
        Panel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {}); QTRY_VERIFY(!Model->IsLoading());
        QVERIFY(Index(Name).isValid()); Model->ChangeDirectory(Index(Name)); QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 15000);
        QVERIFY(Index(Inner).isValid()); Model->ChangeDirectory(Index(Inner)); QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 15000);
        QVERIFY2(!Model->LastNavigationError(), Model->LastNavigationError() ? Model->LastNavigationError()->code_.c_str() : "");
        QVERIFY(!names(Model).empty()); QCOMPARE(Model->WorkspaceLocation().nesting_.size(), 2);
        Model->ChangeDirectory(".."); QTRY_VERIFY(!Model->IsLoading() && Index(Inner).isValid());
        Model->ChangeDirectory(".."); QTRY_VERIFY(!Model->IsLoading() && Index(Name).isValid());
    }
    void builtinUnEggNavigation()
    {
        using namespace hc::plugins;
        const auto Options = BuiltinUnEggOptions();
        const auto Original = hc::settings::AppSettings().Read(hc::settings::Section::Plugins);
        auto* Panel = panels_.front(); auto* Model = static_cast<CFSModel*>(model(Panel)); Model->SetProviderEnabled(true);
        auto Restore = qScopeGuard([&] {
            Panel->ChangeCurrentPath(unc_, {});
            hc::settings::AppSettings().Put(hc::settings::Section::Plugins, Original); TyStPlugInMgr::GetInstance()->Refresh();
        });
        hc::settings::AppSettings().SetPlugins("wcx", {}); TyStPlugInMgr::GetInstance()->Refresh();
        if (!Options.approval_.approved_) {
            QVERIFY(TyStPlugInMgr::GetInstance()->RetrieveWCXByExts("egg").isEmpty()); return;
        }
        const auto Corpus = qEnvironmentVariable("HC_UNEGG_CORPUS"); QVERIFY(!Corpus.isEmpty());
        const auto Root = QFileInfo(Corpus).absolutePath();
        hc::settings::AppSettings().SetPlugins("wcx", QJsonArray{QJsonObject{{"path", "user-priority.wcx64"}, {"extensions", QJsonArray{"egg"}}}});
        TyStPlugInMgr::GetInstance()->Refresh(); const auto Choices = TyStPlugInMgr::GetInstance()->RetrieveWCXByExts("egg");
        QCOMPARE(Choices.size(), 2); QCOMPARE(Choices.front().FileFullPath, QString("user-priority.wcx64")); QCOMPARE(Choices.back().FileFullPath, Options.binaryPath_);
        hc::settings::AppSettings().SetPlugins("wcx", {}); TyStPlugInMgr::GetInstance()->Refresh();
        for (const auto* Name : {"defaults.alz", "defaults.egg"}) {
            QTemporaryDir Directory; QVERIFY(QFile::copy(QDir(Root).filePath(Name), Directory.filePath(Name)));
            Panel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {}); QTRY_VERIFY(!Model->IsLoading());
            QModelIndex Selected;
            for (int Row = 0; Row < Model->rowCount(); ++Row) if (Model->GetName(Model->index(Row, 0)) == Name) Selected = Model->index(Row, 0);
            QVERIFY(Selected.isValid()); Model->ChangeDirectory(Selected); QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 10000);
            QVERIFY2(!Model->LastNavigationError(), Model->LastNavigationError() ? Model->LastNavigationError()->code_.c_str() : "");
            QVERIFY(!names(Model).empty()); const auto Pack = dynamic_cast<nsHC::CFSPack*>(Model->GetRoot().get()); QVERIFY(Pack);
            QCOMPARE(Pack->GetWCX()->PluginPath(), Options.binaryPath_); QVERIFY(!Pack->GetWCX()->WritePolicy().HasValue());
            QVERIFY(!GetModuleHandleW(L"UnEGG64.dll"));
            Panel->ChangeCurrentPath(unc_, {}); QTRY_VERIFY(!Model->IsLoading());
        }
        QVERIFY(hc::settings::AppSettings().Plugins("wcx").isEmpty());
    }
    void builtinSevenZipNavigation()
    {
        using namespace hc::plugins; using namespace hc::storage;
        const auto Corpus = qEnvironmentVariable("HC_SEVENZIP_CORPUS"); QVERIFY(!Corpus.isEmpty());
        const auto Root = QFileInfo(Corpus).absolutePath(); const auto Options = BuiltinSevenZipOptions();
        const auto Original = hc::settings::AppSettings().Read(hc::settings::Section::Plugins);
        auto* Panel = panels_.front(); auto* Model = static_cast<CFSModel*>(model(Panel)); Model->SetProviderEnabled(true);
        auto Restore = qScopeGuard([&] {
            Panel->ChangeCurrentPath(unc_, {});
            hc::settings::AppSettings().Put(hc::settings::Section::Plugins, Original); TyStPlugInMgr::GetInstance()->Refresh();
        });
        hc::settings::AppSettings().SetPlugins("wcx", QJsonArray{QJsonObject{{"path", "user-priority.wcx64"}, {"extensions", QJsonArray{"7z", "zipx", "zx03"}}}});
        TyStPlugInMgr::GetInstance()->Refresh();
        auto Choices = TyStPlugInMgr::GetInstance()->RetrieveWCXByExts("7z"); QCOMPARE(Choices.size(), 2);
        QCOMPARE(Choices.front().FileFullPath, QString("user-priority.wcx64")); QCOMPARE(Choices.back().FileFullPath, Options.binaryPath_);
        Choices = TyStPlugInMgr::GetInstance()->RetrieveWCXByExts("ZIPX", "sample.ZIPX"); QCOMPARE(Choices.size(), 2);
        QCOMPARE(Choices.front().FileFullPath, QString("user-priority.wcx64")); QCOMPARE(Choices.back().FileFullPath, Options.binaryPath_);
        Choices = TyStPlugInMgr::GetInstance()->RetrieveWCXByExts("ZX03", "sample.ZX03"); QCOMPARE(Choices.size(), 2);
        QCOMPARE(Choices.front().FileFullPath, QString("user-priority.wcx64")); QCOMPARE(Choices.back().FileFullPath, Options.binaryPath_);
        hc::settings::AppSettings().SetPlugins("wcx", {}); TyStPlugInMgr::GetInstance()->Refresh();
        for (const auto* Name : {"plain.7z", "plain.zip", "plain.zipx", "test_read_format_rar5_compressed.rar"}) {
            QTemporaryDir Directory; QVERIFY(QFile::copy(QDir(Root).filePath(QString(Name) == "plain.zipx" ? "plain.zip" : Name), Directory.filePath(Name)));
            Panel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {}); QTRY_VERIFY(!Model->IsLoading());
            QModelIndex Selected;
            for (int Row = 0; Row < Model->rowCount(); ++Row) if (Model->GetName(Model->index(Row, 0)) == Name) Selected = Model->index(Row, 0);
            QVERIFY(Selected.isValid()); Model->ChangeDirectory(Selected);
            QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 10000);
            QVERIFY2(!Model->LastNavigationError(), Model->LastNavigationError() ? Model->LastNavigationError()->code_.c_str() : "");
            QVERIFY(!names(Model).empty());
            const auto Pack = dynamic_cast<nsHC::CFSPack*>(Model->GetRoot().get()); QVERIFY(Pack);
            QCOMPARE(Pack->GetWCX()->PluginPath(), Options.binaryPath_);
            QCOMPARE(Pack->GetWCX()->WritePolicy().HasValue(), !QString(Name).endsWith(".rar") && !QString(Name).endsWith(".zipx"));
            QVERIFY(!GetModuleHandleW(L"7z.dll"));
            Panel->ChangeCurrentPath(unc_, {}); QTRY_VERIFY(!Model->IsLoading());
        }
        QFile Fixture(QFINDTESTDATA("fixtures/split-volumes.json")); QVERIFY(Fixture.open(QIODevice::ReadOnly));
        const auto FixtureRoot = QJsonDocument::fromJson(Fixture.readAll()).object(); auto Cases = FixtureRoot["cases"].toArray();
        QFile Zipx(QFINDTESTDATA("fixtures/zipx.json")); QVERIFY(Zipx.open(QIODevice::ReadOnly));
        const auto ZipxRoot = QJsonDocument::fromJson(Zipx.readAll()).object(); Cases.append(ZipxRoot["cases"].toArray()[2]);
        QCOMPARE(Cases.size(), 5); auto* Bridge = TyStCommandMgr::GetInstance()->Bridge(); auto& Scheduler = Bridge->FileJobs()->Scheduler();
        for (const auto& Value : Cases) {
            const auto Case = Value.toObject(); const auto Volumes = Case["volumes"].toArray(); QTemporaryDir Directory, Output;
            for (const auto& Part : Volumes) {
                const auto Volume = Part.toObject(); QFile File(Directory.filePath(Volume["name"].toString())); QVERIFY(File.open(QIODevice::WriteOnly));
                const auto Bytes = QByteArray::fromBase64(Volume["data"].toString().toLatin1()); QCOMPARE(File.write(Bytes), Bytes.size());
            }
            const auto Name = Volumes[Volumes.size() / 2].toObject()["name"].toString();
            Choices = TyStPlugInMgr::GetInstance()->RetrieveWCXByExts(QFileInfo(Name).suffix(), Name); QVERIFY(!Choices.isEmpty());
            QCOMPARE(Choices.front().FileFullPath, Options.binaryPath_);
            Panel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {}); QTRY_VERIFY(!Model->IsLoading()); emit Panel->sig_NotifyPanelActivated();
            const auto Find = [&] { for (int Row = 0; Row < Model->rowCount(); ++Row) if (Model->GetName(Model->index(Row, 0)) == Name) return Model->index(Row, 0); return QModelIndex{}; };
            auto Selected = Find(); QVERIFY(Selected.isValid()); QSet<QString> Previous;
            for (const auto& Job : Scheduler.Snapshots()) Previous.insert(QString::fromStdString(Job.id_));
            bool Seen = false; QTimer Answer; Answer.setInterval(10);
            connect(&Answer, &QTimer::timeout, main_, [&] {
                auto* Dialog = static_cast<hc::ui::ArchiveExtractDialog*>(main_->findChild<QDialog*>("hc.archive.extract")); if (!Dialog) return;
                Seen = true; Answer.stop(); Dialog->findChild<QLineEdit*>("hc.archive.extract.path")->setText(Output.path());
                Dialog->findChild<QCheckBox*>("hc.archive.extract.separate")->setChecked(false);
                Dialog->findChild<QCheckBox*>("hc.archive.extract.queued")->setChecked(false);
                auto* Path = Dialog->findChild<QLineEdit*>("hc.archive.extract.path"); Path->setFocus(); QTest::keyClick(Path, Qt::Key_Return);
            });
            auto* Grid = qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex())); QVERIFY(Grid); Grid->BaseView()->deselectAll();
            bool Focused = false;
            for (int Row = 0; Row < Grid->BaseView()->modelController()->getGridRowCount(); ++Row) {
                Grid->BaseView()->setFocusedRowIndex(Row); auto Index = Grid->BaseView()->focusedRow().modelIndex(0);
                while (const auto* Proxy = qobject_cast<const QAbstractProxyModel*>(Index.model())) Index = Proxy->mapToSource(Index);
                if (Model->GetName(Index) == Name) { Focused = true; break; }
            }
            QVERIFY(Focused); Grid->EnsureKeyboardFocusOnView(); emit Panel->sig_NotifyPanelActivated();
            QTRY_VERIFY(Bridge->Action("hc.archive.extract")->isEnabled());
            Answer.start(); QVERIFY(Bridge->ExecuteCommand("hc.archive.extract", Selected));
            Answer.stop(); QVERIFY(Seen); std::string Job;
            for (const auto& Item : Scheduler.Snapshots()) if (!Previous.contains(QString::fromStdString(Item.id_))) Job = Item.id_;
            QVERIFY(!Job.empty()); QTRY_VERIFY_WITH_TIMEOUT(Scheduler.Snapshot(Job).HasValue() && hc::operations::IsTerminal(Scheduler.Snapshot(Job).Value().state_), 30000);
            const auto Final = Scheduler.Snapshot(Job); QVERIFY2(Final.Value().state_ == hc::operations::JobState::Completed,
                Final.Value().error_ ? Final.Value().error_->code_.c_str() : "split extraction failed");
            const auto Expected = ZipxRoot["files"].toObject();
            for (auto I = Expected.begin(); I != Expected.end(); ++I) {
                QFile File(Output.filePath(I.key())); QVERIFY2(File.open(QIODevice::ReadOnly), qPrintable(File.fileName()));
                QCOMPARE(QCryptographicHash::hash(File.readAll(), QCryptographicHash::Sha256).toHex(), I.value().toObject()["sha256"].toString().toLatin1());
            }
            QTRY_VERIFY(!Bridge->FileJobs()->IsUiPending(Job) && !Model->IsLoading()); Selected = Find(); QVERIFY(Selected.isValid());
            Model->ChangeDirectory(Selected); QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 10000);
            QVERIFY2(!Model->LastNavigationError(), Model->LastNavigationError() ? Model->LastNavigationError()->code_.c_str() : "");
            QVERIFY(names(Model).contains("payload.bin")); hc::workspace::TabState Tab;
            Tab.location_ = Model->WorkspaceLocation(); Tab.provider_ = Options.manifestPath_;
            Panel->ChangeCurrentPath(unc_, {}); QTRY_VERIFY(!Model->IsLoading());
            RestoreWorkspaceLocation(Model, Tab); QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading() && names(Model).contains("payload.bin"), 10000);
            QVERIFY(names(Model).contains("payload.bin")); QCOMPARE(Model->WorkspaceLocation().root_, Tab.location_.root_);
            Panel->ChangeCurrentPath(unc_, {}); QTRY_VERIFY(!Model->IsLoading());
        }
        QVERIFY(hc::settings::AppSettings().Plugins("wcx").isEmpty());
    }
    void wcxHostApprovalUi_data()
    {
        QTest::addColumn<bool>("Approve"); QTest::addColumn<bool>("RevokeDuringConfirmation"); QTest::addColumn<bool>("Wlx");
        for (const bool Wlx : {false, true})
        {
            const QByteArray Prefix = Wlx ? "wlx-" : "wcx-";
            QTest::newRow(Prefix + "approve") << true << false << Wlx;
            QTest::newRow(Prefix + "decline") << false << false << Wlx;
            QTest::newRow(Prefix + "revoke-pending-approval") << true << true << Wlx;
        }
    }
    void wcxHostApprovalUi()
    {
        QFETCH(bool, Approve); QFETCH(bool, RevokeDuringConfirmation); QFETCH(bool, Wlx);
        QTemporaryDir Directory; QVERIFY(Directory.isValid()); TyPlugInWCX Spec; QVERIFY(configureWcx(Spec, Directory, true));
        if (Wlx)
        {
            hc::plugins::PluginViewerOptions Options; QVERIFY(configureWlx(Options, Directory));
            Spec.FileFullPath = Options.binaryPath_; Spec.hostManifest_ = Options.manifestPath_; Spec.hostApproval_ = Options.approval_;
        }
        hc::settings::Repository Settings(Directory.path());
        const QString Group = Wlx ? "wlx" : "wcx"; const QString Prefix = Wlx ? "wlx" : "wcx";
        Settings.SetPlugins(Group, QJsonArray{QJsonObject{{"path",Spec.FileFullPath},{"detect",Wlx ? "EXT=\"TXT\"" : ""},{"extensions",QJsonArray{"hcwcx"}}}});
        QPluginHostSettings Widget(Wlx ? hc::plugins::PluginFormat::Wlx : hc::plugins::PluginFormat::Wcx);
        Widget.Load(Settings); Widget.resize(650, 180); Widget.show();
        const auto Expected = hc::plugins::InspectPluginBinary(Spec.FileFullPath, Wlx ? hc::plugins::PluginFormat::Wlx : hc::plugins::PluginFormat::Wcx); QVERIFY(Expected.HasValue());
        auto* Review = Widget.findChild<QPushButton*>(Prefix + "HostReview"); QVERIFY(Review);
        auto* Revoke = Widget.findChild<QPushButton*>(Prefix + "HostRevoke"); QVERIFY(Revoke);
        const bool PreviousNative = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
        const auto Restore = qScopeGuard([&] { QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, PreviousNative); });
        bool Confirmed = false, TimedOut = false, DefaultNo = false, UiPermission = false;
        QElapsedTimer Elapsed; Elapsed.start(); QTimer Driver; Driver.setInterval(20);
        connect(&Driver, &QTimer::timeout, &Widget, [&] {
            if (Elapsed.elapsed() > 10000)
            {
                TimedOut = true;
                for (auto* Dialog : Widget.findChildren<QDialog*>()) if (Dialog->isVisible()) Dialog->reject();
                return;
            }
            for (auto* Message : Widget.findChildren<QMessageBox*>()) if (Message->isVisible() && !Confirmed)
            {
                Confirmed = true; DefaultNo = Message->defaultButton() == Message->button(QMessageBox::No);
                UiPermission = Message->text().contains("플러그인 창 표시");
                if (RevokeDuringConfirmation) Revoke->click();
                auto* Choice = Message->button(Approve ? QMessageBox::Yes : QMessageBox::No);
                if (Choice) Choice->click(); else Message->reject();
            }
        });
        Driver.start(); QTest::mouseClick(Review, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(Confirmed || TimedOut, 12000); Driver.stop(); QVERIFY(!TimedOut); QVERIFY(DefaultNo); QTRY_VERIFY(Review->isEnabled());
        Widget.Save(Settings);
        if (Wlx) QVERIFY(UiPermission);
        const auto Reviewed = hc::settings::JsonBytes(Settings.Plugins(Group)[0].toObject()["hostReviewed"]);
        if (Approve && !RevokeDuringConfirmation)
        {
            auto Parsed = hc::plugins::ParseManifest(Reviewed); QVERIFY(Parsed.HasValue());
            QVERIFY(Parsed.Value() == Expected.Value().manifest_);
            QVERIFY(Settings.Plugins(Group)[0].toObject()["hostEnabled"].toBool());
        }
        else QVERIFY(Settings.Plugins(Group)[0].toObject()["hostReviewed"].toObject().isEmpty());
        if (RevokeDuringConfirmation) QVERIFY(Settings.Plugins(Group)[0].toObject()["hostEnabled"].toBool());
    }
    void wlxViewerCommandsUi_data()
    {
        QTest::addColumn<bool>("Automatic"); QTest::addColumn<bool>("NativeKeys");
        QTest::newRow("manual") << false << true; QTest::newRow("automatic") << true << true;
        // Content/approval coverage remains independent of desktop foreground
        // availability. The two native-input rows still enforce real key routing.
        QTest::newRow("automatic-content") << true << false;
    }
    void wlxViewerCommandsUi()
    {
        using namespace hc::plugins;
        QFETCH(bool, Automatic); QFETCH(bool, NativeKeys);
        QTemporaryDir Package; PluginViewerOptions Options; QVERIFY(configureWlx(Options, Package));
        const auto Original = hc::settings::AppSettings().Read(hc::settings::Section::Plugins);
        auto* Host = static_cast<QtViewerHost*>(main_->findChild<QObject*>("hc.viewer.host")); QVERIFY(Host);
        const auto Restore = qScopeGuard([&] {
            Host->CloseAll();
            // Data rows also need isolation after an assertion fails: otherwise
            // findChild can select the previous row's closed viewer surface.
            for (auto* Window : main_->findChildren<QDialog*>("hc.viewer.full")) Window->close();
            for (auto* Preview : main_->findChildren<CQuickView*>())
                QMetaObject::invokeMethod(Preview, "CloseRequested", Qt::DirectConnection);
            while (!Host->IsIdle()) QTest::qWait(10);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            hc::settings::AppSettings().Put(hc::settings::Section::Plugins, Original); TyStPlugInMgr::GetInstance()->Refresh();
        });
        { auto Plugins = hc::settings::AppSettings().Read(hc::settings::Section::Plugins).toObject(); Plugins["viewerSelection"] = Automatic ? "automatic" : "manual"; hc::settings::AppSettings().Put(hc::settings::Section::Plugins, Plugins); }
        // Install via the actual registration UI. The fixture deliberately has
        // no ListGetDetectString export: registration must never execute it.
        hc::settings::AppSettings().SetPlugins("wlx", {}); SetPluginField(hc::settings::AppSettings(), "wlx", 0, "path", "unrelated.wlx64");
        SetPluginField(hc::settings::AppSettings(), "wlx", 0, "hostEnabled", false); hc::settings::AppSettings().Commit(); TyStPlugInMgr::GetInstance()->Refresh();
        {
            QMainOpts Dialog(main_); Dialog.show(); QCoreApplication::processEvents();
            Dialog.findChild<QStackedWidget*>("stkOpts")->setCurrentWidget(Dialog.findChild<QWidget*>("pgPluginManage"));
            const bool Previous = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
            QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
            const auto RestoreDialogs = qScopeGuard([&] { QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, Previous); });
            bool Chosen = false, TimedOut = false; QTimer Driver; Driver.setInterval(20); QElapsedTimer Wait; Wait.start();
            connect(&Driver, &QTimer::timeout, &Dialog, [&] {
                if (auto* Kind = Dialog.findChild<QDialog*>("pluginAddType"); Kind && Kind->isVisible()) { Kind->findChild<QComboBox*>()->setCurrentIndex(0); Kind->accept(); return; }
                for (auto* Picker : Dialog.findChildren<QFileDialog*>()) if (Picker->isVisible())
                {
                    if (Wait.elapsed() > 5000)
                    {
                        TimedOut = true; qWarning() << "WLX picker timeout" << Picker->directory().absolutePath() << Picker->selectedFiles();
                        Picker->reject(); continue;
                    }
                    if (!Chosen) { Chosen = true; Picker->setDirectory(Package.path()); }
                    if (auto* Name = Picker->findChild<QLineEdit*>("fileNameEdit")) Name->setText(Options.binaryPath_);
                    // QFileDialog's directory model loads asynchronously. An
                    // early accept can be ignored until the selected file exists
                    // in that model, especially in the optimized product build.
                    QMetaObject::invokeMethod(Picker, "accept", Qt::QueuedConnection);
                }
            });
            Driver.start(); Dialog.findChild<QPushButton*>("pluginAdd")->click(); Driver.stop(); QVERIFY(Chosen); QVERIFY(!TimedOut);
            QCOMPARE(TyStPlugInMgr::GetInstance()->GetWLXCount(), 1u);
            const auto Draft = Dialog.findChild<QPluginManagementPage*>()->Document()["wlx"].toArray(); QCOMPARE(Draft.size(), 2);
            QVERIFY(Draft[1].toObject()["hostEnabled"].toBool());
            const auto Native = QDir::toNativeSeparators(Options.binaryPath_).toStdWString(); QVERIFY(!GetModuleHandleW(Native.c_str()));
            QVERIFY(Dialog.SaveSettings());
        }
        SetPluginField(hc::settings::AppSettings(), "wlx", 1, "hostManifest", Options.manifestPath_);
        SetPluginField(hc::settings::AppSettings(), "wlx", 1, "hostReviewed", hc::settings::JsonObject(EncodeManifest(Options.approval_.reviewed_))); hc::settings::AppSettings().Commit(); TyStPlugInMgr::GetInstance()->Refresh();
        {
            QMainOpts Dialog(main_); Dialog.show(); QCoreApplication::processEvents();
            Dialog.findChild<QStackedWidget*>("stkOpts")->setCurrentWidget(Dialog.findChild<QWidget*>("pgPluginManage"));
            auto* Catalog = Dialog.findChild<QTreeWidget*>("pluginCatalog"); QVERIFY(Catalog);
            for (QTreeWidgetItemIterator Item(Catalog); *Item; ++Item) if ((*Item)->data(0, Qt::UserRole + 1).toString() == "1:0") Catalog->setCurrentItem(*Item);
            QTimer Confirm; Confirm.setInterval(10); connect(&Confirm, &QTimer::timeout, &Dialog, [&] { for (auto* Message : Dialog.findChildren<QMessageBox*>()) if (Message->isVisible()) if (auto* Yes = Message->button(QMessageBox::Yes)) Yes->click(); }); Confirm.start();
            Dialog.findChild<QPushButton*>("pluginRemove")->click(); Confirm.stop();
            QCOMPARE(TyStPlugInMgr::GetInstance()->GetWLXCount(), 2u); QVERIFY(Dialog.SaveSettings());
            QCOMPARE(TyStPlugInMgr::GetInstance()->GetWLXCount(), 1u);
            QCOMPARE(hc::settings::AppSettings().Plugins("wlx")[0].toObject()["path"].toString(), Options.binaryPath_);
            QVERIFY(hc::settings::AppSettings().Plugins("wlx")[0].toObject()["hostEnabled"].toBool());
            QCOMPARE(hc::settings::AppSettings().Plugins("wlx")[0].toObject()["hostReviewed"].toObject(), hc::settings::JsonObject(EncodeManifest(Options.approval_.reviewed_)));
            QVERIFY(Dialog.SaveSettings());
        }
        hc::settings::AppSettings().SetPlugins("wlx", {}); SetPluginField(hc::settings::AppSettings(), "wlx", 0, "path", Options.binaryPath_);
        SetPluginField(hc::settings::AppSettings(), "wlx", 0, "hostManifest", Options.manifestPath_);
        SetPluginField(hc::settings::AppSettings(), "wlx", 0, "hostEnabled", true);
        SetPluginField(hc::settings::AppSettings(), "wlx", 0, "hostReviewed", hc::settings::JsonObject(EncodeManifest(Options.approval_.reviewed_))); hc::settings::AppSettings().Commit();
        TyStPlugInMgr::GetInstance()->Refresh();
        QTemporaryDir Content(QDir(localOnly_ ? QDir::tempPath() : unc_).filePath("hc-wlx-XXXXXX")); QVERIFY(Content.isValid());
        qInfo() << "WLX product content:" << Content.path() << "SMB:" << !localOnly_;
        { QFile File(Content.filePath("뷰어.txt")); QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write("WLX product content"), 19); }
        auto* Source = panels_.front(); auto* Target = panels_.back();
        for (auto* Panel : panels_)
        {
            static_cast<CFSModel*>(model(Panel))->SetProviderEnabled(true);
            Panel->ChangeCurrentPath(QDir::toNativeSeparators(Content.path()), {}); QTRY_VERIFY_WITH_TIMEOUT(!model(Panel)->IsLoading(), 5000);
        }
        auto* Model = model(Source); auto* Grid = qobject_cast<CGridView*>(Source->GetFocusView(Source->CurrentTabIndex())); QVERIFY(Grid);
        QModelIndex Selected;
        for (int Row = 0; Row < Grid->BaseView()->modelController()->getGridRowCount(); ++Row)
        {
            Grid->BaseView()->setFocusedRowIndex(Row); auto Index = Grid->BaseView()->focusedRow().modelIndex(0);
            while (const auto Proxy = qobject_cast<const QAbstractProxyModel*>(Index.model())) Index = Proxy->mapToSource(Index);
            if (Model->GetName(Index) == "뷰어.txt") { Selected = Index; break; }
        }
        QVERIFY(Selected.isValid()); Grid->EnsureKeyboardFocusOnView(); QVERIFY(QApplication::focusWidget());
        QVERIFY(legacyOpen(static_cast<CFSModel*>(Model), Selected));
        QTRY_VERIFY_WITH_TIMEOUT(main_->findChild<QDialog*>("hc.viewer.full"), 5000);
        QPointer<QDialog> Full = main_->findChild<QDialog*>("hc.viewer.full");
        auto* Surface = Full->findChild<QtViewerSurface*>(); QVERIFY(Surface);
        QTRY_VERIFY2_WITH_TIMEOUT(Surface->IsReady(), Surface->Snapshot().error_
            ? Surface->Snapshot().error_->code_.c_str() : "viewer did not become ready", 5000);
        QCOMPARE(Surface->findChild<QtPluginViewerSurface*>()->IsReady(), Automatic);
        const auto SelectWlx = [](QtViewerSurface* Surface) {
            auto* Menu = Surface->findChild<QMenu*>("hc.viewer.plugins"); if (!Menu || !Menu->menuAction()->isVisible()) return false;
            QMetaObject::invokeMethod(Menu, "aboutToShow", Qt::DirectConnection);
            auto* Choice = Menu->findChild<QAction*>("hc.viewer.plugin.0"); if (!Choice) return false;
            Choice->trigger(); return true;
        };
        QVERIFY(SelectWlx(Surface)); QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(), 5000);
        QVERIFY(Surface->findChild<QtPluginViewerSurface*>()->IsReady()); QCOMPARE(Surface->Snapshot().source_.name_, std::string("뷰어.txt"));
        QVERIFY(Full->screen()->grabWindow(Full->winId()).save(QCoreApplication::applicationDirPath() + "/wlx-product-full.png"));
        if (NativeKeys) QVERIFY(sendWlxKey(Surface, VK_ESCAPE, false));
        else Full->close();
        QTRY_VERIFY(Full.isNull()); QTRY_VERIFY_WITH_TIMEOUT(Host->IsIdle(), 5000);
        Grid->EnsureKeyboardFocusOnView(); QTest::keyClick(QApplication::focusWidget(), Qt::Key_Q, Qt::ControlModifier);
        QTRY_VERIFY_WITH_TIMEOUT(qobject_cast<CQuickView*>(Target->GetFocusView(Target->CurrentTabIndex())), 5000);
        QPointer<CQuickView> Quick = qobject_cast<CQuickView*>(Target->GetFocusView(Target->CurrentTabIndex()));
        Surface = Quick->findChild<QtViewerSurface*>(); QVERIFY(Surface); QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(), 5000);
        QCOMPARE(Surface->findChild<QtPluginViewerSurface*>()->IsReady(), Automatic);
        QVERIFY(SelectWlx(Surface)); QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(), 5000); QVERIFY(Surface->findChild<QtPluginViewerSurface*>()->IsReady());
        QVERIFY(main_->screen()->grabWindow(main_->winId()).save(QCoreApplication::applicationDirPath() + "/wlx-product-quick.png"));
        Surface->findChild<QAction*>("hc.viewer.fallback")->trigger(); QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(), 5000);
        QCOMPARE(Surface->findChild<QPlainTextEdit*>()->toPlainText(), QString("WLX product content"));
        {
            QMainOpts Dialog(main_); Dialog.show(); QCoreApplication::processEvents();
            Dialog.findChild<QStackedWidget*>("stkOpts")->setCurrentWidget(Dialog.findChild<QWidget*>("pgPluginManage"));
            auto* Catalog = Dialog.findChild<QTreeWidget*>("pluginCatalog"); QVERIFY(Catalog);
            for (QTreeWidgetItemIterator Item(Catalog); *Item; ++Item) if ((*Item)->data(0, Qt::UserRole + 1).toString() == "1:0") Catalog->setCurrentItem(*Item);
            auto* Tabs = Dialog.findChild<QTabWidget*>("pluginInspectorTabs"); QVERIFY(Tabs); Tabs->setCurrentIndex(1);
            auto* Table = Dialog.findChild<QTableWidget*>("wlxHostTable"); QVERIFY(Table); QCOMPARE(Table->rowCount(), 1);
            Dialog.findChild<QPushButton*>("wlxHostRevoke")->click();
            Dialog.SaveSettings(); hc::settings::AppSettings().Commit();
            QVERIFY(hc::settings::AppSettings().Plugins("wlx")[0].toObject()["hostReviewed"].toObject().isEmpty());
            QVERIFY(hc::settings::AppSettings().Plugins("wlx")[0].toObject()["hostEnabled"].toBool());
            QVERIFY(Dialog.grab().save(QCoreApplication::applicationDirPath() + "/wlx-product-options.png"));
        }
        QSignalSpy Failed(Surface, &QtViewerSurface::Failed); QVERIFY(SelectWlx(Surface));
        QTRY_COMPARE_WITH_TIMEOUT(Failed.size(), 1, 5000); QVERIFY(!Surface->IsReady()); QTRY_VERIFY_WITH_TIMEOUT(Host->IsIdle(), 5000);
        QMenu Legacy; QCOMPARE(TyStPlugInMgr::GetInstance()->ConstructWLXMenu(&Legacy, Content.filePath("뷰어.txt")), 0u);
        const auto Native = QDir::toNativeSeparators(Options.binaryPath_).toStdWString(); QVERIFY(!GetModuleHandleW(Native.c_str()));
        SetPluginField(hc::settings::AppSettings(), "wlx", 0, "hostReviewed", hc::settings::JsonObject(EncodeManifest(Options.approval_.reviewed_))); hc::settings::AppSettings().Commit();
        QVERIFY(SelectWlx(Surface)); QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(), 5000); QVERIFY(Surface->findChild<QtPluginViewerSurface*>()->IsReady());
        if (NativeKeys) QVERIFY(sendWlxKey(Surface, 'Q', true));
        else QVERIFY(QMetaObject::invokeMethod(Surface, "CloseRequested", Qt::DirectConnection));
        QTRY_VERIFY(Quick.isNull()); QTRY_VERIFY_WITH_TIMEOUT(Host->IsIdle(), 5000);
        for (auto* Panel : panels_) { Panel->ChangeCurrentPath(unc_, {}); QTRY_VERIFY_WITH_TIMEOUT(!model(Panel)->IsLoading(), 5000); }
    }
    void wcxHostSettingsPersistence()
    {
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        hc::settings::Repository Settings(Directory.path());
        using Selection = QtViewerSurface::PluginViewerSelectionMode;
        QCOMPARE(QPluginHostSettings::ViewerSelection(Settings), Selection::Manual);
        {
            QPluginHostSettings Wlx(hc::plugins::PluginFormat::Wlx); Wlx.Load(Settings);
            auto* Mode = Wlx.findChild<QComboBox*>("wlxSelectionMode"); QVERIFY(Mode);
            Mode->setCurrentIndex(1);
            QCOMPARE(QPluginHostSettings::ViewerSelection(Settings), Selection::Manual);
            Wlx.Save(Settings); Settings.Commit();
            hc::settings::Repository Reload(QFileInfo(Settings.FileName()).absolutePath());
            QCOMPARE(QPluginHostSettings::ViewerSelection(Reload), Selection::Automatic);
            Wlx.Load(Reload); QCOMPARE(Mode->currentIndex(), 1);
            Mode->setCurrentIndex(2); Wlx.Save(Reload); Reload.Commit();
            QCOMPARE(QPluginHostSettings::ViewerSelection(Reload), Selection::Force);
            auto Plugins = Reload.Read(hc::settings::Section::Plugins).toObject(); Plugins["viewerSelection"] = "manual"; Reload.Put(hc::settings::Section::Plugins, Plugins); Wlx.Load(Reload);
            QCOMPARE(Mode->currentIndex(), 0);
            Wlx.resize(720, 240); Wlx.show(); QCoreApplication::processEvents();
            QVERIFY(Wlx.grab().save(QCoreApplication::applicationDirPath() + "/wlx-selection-settings.png"));
        }
        hc::plugins::PluginManifest Manifest; Manifest.id_ = "hc.test.settings"; Manifest.version_ = "1";
        Manifest.binary_ = "fixture.wcx64"; Manifest.sha256_ = std::string(64, 'a');
        Manifest.extensions_ = {hc::plugins::Extension::ArchiveRead}; Manifest.permissions_ = {hc::plugins::Permission::ReadContent};
        const auto Reviewed = hc::plugins::EncodeManifest(Manifest);
        Settings.Reload();
        Settings.SetPlugins("wcx", QJsonArray{QJsonObject{{"path","fixture.wcx64"},{"extensions",QJsonArray{"hcwcx"}},{"hostEnabled",true},{"hostManifest","plugin.json"},{"hostReviewed",hc::settings::JsonObject(Reviewed)}}});
        {
            QPluginHostSettings Widget; Widget.Load(Settings); Widget.resize(650, 180); Widget.show();
            auto* Table = Widget.findChild<QTableWidget*>("wcxHostTable"); QVERIFY(Table); QCOMPARE(Table->rowCount(), 1);
            auto* Revoke = Widget.findChild<QPushButton*>("wcxHostRevoke"); QVERIFY(Revoke);
            QTest::mouseClick(Revoke, Qt::LeftButton);
            QCOMPARE(Table->item(0, 2)->checkState(), Qt::Checked);
            // Closing/cancelling the options UI must not persist its draft.
        }
        QCOMPARE(Settings.Plugins("wcx")[0].toObject()["hostReviewed"].toObject(), hc::settings::JsonObject(Reviewed));
        QPluginHostSettings Widget; Widget.Load(Settings); Widget.resize(650, 180); Widget.show();
        auto* Table = Widget.findChild<QTableWidget*>("wcxHostTable"); QVERIFY(Table);
        auto* Revoke = Widget.findChild<QPushButton*>("wcxHostRevoke"); QVERIFY(Revoke); QTest::mouseClick(Revoke, Qt::LeftButton);
        Widget.Save(Settings); Settings.Commit();
        hc::settings::Repository Reload(QFileInfo(Settings.FileName()).absolutePath());
        QVERIFY(Reload.Plugins("wcx")[0].toObject()["hostReviewed"].toObject().isEmpty());
        QVERIFY(Reload.Plugins("wcx")[0].toObject()["hostEnabled"].toBool());
        QCOMPARE(Reload.Plugins("wcx")[0].toObject()["path"].toString(), QString("fixture.wcx64"));
        Widget.Load(Reload); QCOMPARE(Table->item(0, 2)->checkState(), Qt::Checked);
        QVERIFY(Widget.grab().save(QCoreApplication::applicationDirPath() + "/wcx-host-settings.png"));
    }
    void dopusPluginsUi()
    {
        using namespace hc::plugins; using namespace hc::storage; using namespace hc::operations;
        QTemporaryDir Package, Root, Copied; const auto Binary = Package.filePath("dopus.dll");
        QVERIFY(QFile::copy(QCoreApplication::applicationDirPath() + "/hc_dopus_fixture.dll", Binary));
        QFile Dll(Binary); QVERIFY(Dll.open(QIODevice::ReadOnly)); const auto Hash = QCryptographicHash::hash(Dll.readAll(), QCryptographicHash::Sha256).toHex().toStdString(); Dll.close();
        { QFile Input(Root.filePath("sample.bitmap")); QVERIFY(Input.open(QIODevice::WriteOnly)); QCOMPARE(Input.write("dopus product"), 13); }
        const auto Saved = hc::settings::AppSettings().Read(hc::settings::Section::Plugins);
        auto* Panel = panels_.front(); Panel->sig_NotifyPanelActivated(); const auto PreviousTab = Panel->CurrentTabIndex();
        auto* Host = static_cast<QtViewerHost*>(main_->findChild<QObject*>("hc.viewer.host")); QVERIFY(Host);
        const auto Restore = qScopeGuard([&] {
            Host->CloseAll(); for (auto* Dialog : main_->findChildren<QDialog*>("hc.viewer.full")) Dialog->close();
            for (auto* Dialog : main_->findChildren<QDialog*>("hc.dataPluginDialog")) Dialog->close();
            while (!Host->IsIdle()) QTest::qWait(5);
            if (Panel->CurrentTabIndex() != PreviousTab) Panel->CloseTab();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); QThreadPool::globalInstance()->waitForDone();
            hc::settings::AppSettings().Put(hc::settings::Section::Plugins, Saved);
        });
        for (const auto Format : {PluginFormat::DopusVfs, PluginFormat::DopusViewer})
        {
            const bool Vfs = Format == PluginFormat::DopusVfs; const auto Group = Vfs ? QString("dopusVfs") : QString("dopusViewer");
            PluginManifest Manifest; Manifest.id_ = "hc.dopus.product"; Manifest.version_ = "1"; Manifest.binary_ = "dopus.dll"; Manifest.sha256_ = Hash; Manifest.format_ = Format;
            Manifest.extensions_ = Vfs ? std::set{Extension::StorageRead, Extension::StorageWrite} : std::set{Extension::Viewer, Extension::Metadata};
            Manifest.permissions_ = {Permission::ReadContent, Vfs ? Permission::WriteContent : Permission::UserInterface};
            const auto Path = Package.filePath(Vfs ? "vfs.json" : "viewer.json"); QFile Json(Path); QVERIFY(Json.open(QIODevice::WriteOnly)); Json.write(EncodeManifest(Manifest)); Json.close();
            hc::settings::AppSettings().SetPlugins(Group, QJsonArray{QJsonObject{{"path",Binary},{"hostEnabled",true},{"hostManifest",Path},{"hostReviewed",hc::settings::JsonObject(EncodeManifest(Manifest))}}});
            QPluginHostSettings Settings(Format); Settings.Load(hc::settings::AppSettings()); Settings.Save(hc::settings::AppSettings());
            const auto Choices = QPluginHostSettings::DataChoices(hc::settings::AppSettings(), Format); QCOMPARE(Choices.size(), 1u); QVERIFY(Choices.front().options_.approval_.approved_);
            QCOMPARE(Choices.front().options_.binaryPath_, Binary);
        }
        QTemporaryDir WdxPackage; const auto WdxBinary = WdxPackage.filePath("data.dll"); QVERIFY(QFile::copy(QCoreApplication::applicationDirPath() + "/hc_tc_data_fixture.dll", WdxBinary));
        QFile WdxFile(WdxBinary); QVERIFY(WdxFile.open(QIODevice::ReadOnly)); PluginManifest Wdx;
        Wdx.id_ = "mixed.wdx"; Wdx.version_ = "1"; Wdx.binary_ = "data.dll"; Wdx.format_ = PluginFormat::Wdx; Wdx.extensions_ = {Extension::Metadata}; Wdx.permissions_ = {Permission::ReadContent};
        Wdx.sha256_ = QCryptographicHash::hash(WdxFile.readAll(), QCryptographicHash::Sha256).toHex().toStdString(); WdxFile.close();
        QFile WdxJson(WdxPackage.filePath("wdx.json")); QVERIFY(WdxJson.open(QIODevice::WriteOnly)); WdxJson.write(EncodeManifest(Wdx)); WdxJson.close();
        hc::settings::AppSettings().SetPlugins("wdx", QJsonArray{QJsonObject{{"path",WdxBinary},{"hostEnabled",true},{"hostManifest",WdxJson.fileName()},{"hostReviewed",hc::settings::JsonObject(EncodeManifest(Wdx))}}});
        QSignalSpy ColumnCatalog(&hc::columns::PluginFields::Instance(), &hc::columns::PluginFields::Changed);
        hc::columns::PluginFields::Instance().Refresh(); QTRY_VERIFY_WITH_TIMEOUT(ColumnCatalog.count(), 10000);
        QString ViewerField, WdxField;
        for (const auto& Field : hc::columns::PluginFields::Instance().Fields()) if (Field.id_.startsWith("wdx.") && Field.id_.endsWith(".0.0")) WdxField = Field.id_;
        QVERIFY2(!WdxField.isEmpty(), qPrintable(hc::columns::PluginFields::Instance().Status()));
        for (const auto& Field : hc::columns::PluginFields::Instance().Fields()) if (Field.id_.startsWith("dopusviewer.") && Field.id_.endsWith(".1.0")) ViewerField = Field.id_;
        QVERIFY(!ViewerField.isEmpty());
        auto MetadataRead = QtConcurrent::run([Load = hc::columns::PluginFields::Instance().Loader(), Path = Root.filePath("sample.bitmap"), ViewerField] { return Load(Path, ViewerField, {}); });
        QTRY_VERIFY(MetadataRead.isFinished()); QCOMPARE(MetadataRead.result()[ViewerField].Raw().toLongLong(), 2);
        hc::settings::AppSettings().SetPlugins("wlx", {});
        auto Plugins = hc::settings::AppSettings().Read(hc::settings::Section::Plugins).toObject(); Plugins["viewerSelection"] = "automatic"; hc::settings::AppSettings().Put(hc::settings::Section::Plugins, Plugins);
        const auto ViewerChoices = QPluginHostSettings::ViewerChoices(hc::settings::AppSettings()); QCOMPARE(ViewerChoices.size(), 1u); QCOMPARE(ViewerChoices.front().id_, QString("dopus-0"));
        QTimer Driver; Driver.setInterval(10); QElapsedTimer Timeout; Timeout.start();
        connect(&Driver, &QTimer::timeout, main_, [&] {
            for (auto* Widget : QApplication::topLevelWidgets()) if (auto* Input = qobject_cast<QInputDialog*>(Widget))
            {
                if (!Input->isVisible()) continue;
                if (Timeout.elapsed() > 5000) { Input->reject(); continue; }
                if (Input->labelText().contains(tr("전체 경로"))) Input->setTextValue(Root.path());
                Input->accept();
            }
        });
        Driver.start(); auto* Open = main_->findChild<QAction*>("hc.menu.dopus.open"); QVERIFY(Open); Open->trigger(); Driver.stop();
        QTRY_VERIFY_WITH_TIMEOUT(Panel->CurrentTabIndex() != PreviousTab, 5000);
        auto* Model = static_cast<CFSModel*>(model(Panel)); QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 5000); QVERIFY(names(Model).contains("sample.bitmap"));
        auto* Grid = qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex())); QVERIFY(Grid); QModelIndex Selected;
        const auto VfsColumns = Model->SnapshotProvider()->Columns(); QCOMPARE(VfsColumns.size(), 3u);
        auto ColumnProfile = hc::columns::DefaultProfile(); ColumnProfile.columns_[1].fieldId_ = QString::fromStdString(VfsColumns[0].key_);
        ColumnProfile.columns_[2].fieldId_ = QString::fromStdString(VfsColumns[1].key_); ColumnProfile.columns_[3].fieldId_ = ViewerField;
        hc::columns::ScriptDefinition VfsScript; VfsScript.title_ = "VFS derived"; VfsScript.dependencies_ = {ColumnProfile.columns_[1].fieldId_};
        VfsScript.outputs_ = {{"double", "Double", hc::columns::FieldType::Integer}};
        VfsScript.source_ = "return function(item) return {double=item['" + ColumnProfile.columns_[1].fieldId_ + "']*2} end"; ColumnProfile.scripts_.append(VfsScript);
        ColumnProfile.columns_[4].fieldId_ = hc::columns::ScriptPrefix(VfsScript) + "double"; ColumnProfile.columns_[4].widths_[0].hidden_ = false;
        ColumnProfile.columns_[5].fieldId_ = WdxField; ColumnProfile.columns_[5].widths_[0].hidden_ = false;
        ColumnProfile.sorts_ = {{ColumnProfile.columns_[4].id_, Qt::DescendingOrder}, {ColumnProfile.columns_[1].id_, Qt::AscendingOrder}};
        QVERIFY(Grid->ApplyColumnProfile(ColumnProfile));
        for (int Row = 0; Row < Model->rowCount(); ++Row) if (Model->GetName(Model->index(Row, 0)) == "sample.bitmap") {
            QCOMPARE(Model->index(Row, 2).data().toString(), QString("VFS value 한글"));
            QCOMPARE(Model->index(Row, 1).data(CFsModelT::USR_ROLE_SORT).toULongLong(), quint64(13));
            QCOMPARE(Model->index(Row, 3).data(CFSModel::ColumnStateRole).toInt(), int(hc::columns::ValueState::Unsupported)); // No extraction to feed the viewer.
        }
        {
            QMainOpts ColumnsDialog(main_); ColumnsDialog.SetListView(Grid); ColumnsDialog.ShowListSettingsPage(); ColumnsDialog.show(); QCoreApplication::processEvents();
            auto* FieldChoice = ColumnsDialog.findChild<QComboBox*>("hc.columns.field"); QVERIFY(FieldChoice->findData(ColumnProfile.columns_[2].fieldId_) >= 0);
            ColumnsDialog.findChild<QComboBox*>("hc.columns.sampleMode")->setCurrentIndex(1);
            auto* Preview = ColumnsDialog.findChild<QTableWidget*>("hc.columns.preview"); QCOMPARE(Preview->rowCount(), 1);
            auto* Calculate = ColumnsDialog.findChild<QPushButton*>("hc.columns.calculate"); Calculate->click();
            QTRY_COMPARE(Preview->item(0, 4)->text(), QString("26")); QTRY_COMPARE(Preview->item(0, 3)->text(), QString("지원 안 함"));
            ColumnsDialog.findChild<QCheckBox*>("hc.columns.extractSamples")->setChecked(true); Calculate->click();
            QTRY_VERIFY2_WITH_TIMEOUT(Preview->item(0, 3)->text() == "2", qPrintable(Preview->item(0, 3)->toolTip()), 15000);
            QTRY_COMPARE_WITH_TIMEOUT(Preview->item(0, 5)->text(), QString("9007199254740993"), 15000);
            QCOMPARE(Preview->item(0, 1)->text(), QString("13")); QCOMPARE(Preview->item(0, 4)->text(), QString("26"));
            QVERIFY(ColumnsDialog.grab().save(QCoreApplication::applicationDirPath() + "/columns-mixed-providers.png")); ColumnsDialog.reject();
        }
        for (int Row = 0; Row < Grid->BaseView()->modelController()->getGridRowCount(); ++Row)
        {
            Grid->BaseView()->setFocusedRowIndex(Row); auto Index = Grid->BaseView()->focusedRow().modelIndex(0);
            while (const auto Proxy = qobject_cast<const QAbstractProxyModel*>(Index.model())) Index = Proxy->mapToSource(Index);
            if (Model->GetName(Index) == "sample.bitmap") { Selected = Index; break; }
        }
        QVERIFY(Selected.isValid()); Grid->EnsureKeyboardFocusOnView(); Panel->sig_NotifyPanelActivated();
        auto* Bridge = TyStCommandMgr::GetInstance()->Bridge();
        auto Copy = Bridge->Dispatch("hc.file.copy", {{"destination", Copied.path()}}, "test", Selected);
        QTRY_VERIFY(Copy.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready); const auto Accepted = Copy.get(); QVERIFY(Accepted.HasValue());
        auto& Scheduler = Bridge->FileJobs()->Scheduler(); QTRY_VERIFY_WITH_TIMEOUT(IsTerminal(Scheduler.Snapshot(Accepted.Value().jobId_).Value().state_), 5000);
        QCOMPARE(Scheduler.Snapshot(Accepted.Value().jobId_).Value().state_, JobState::Completed);
        QFile CopyFile(Copied.filePath("sample.bitmap")); QVERIFY(CopyFile.open(QIODevice::ReadOnly)); QCOMPARE(CopyFile.readAll(), QByteArray("dopus product")); CopyFile.close();
        // Copy completion refreshes the panel model. Reacquire its index before
        // invoking a selection-based menu; the pre-copy index is no longer valid.
        QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 5000);
        Selected = {};
        for (int Row = 0; Row < Grid->BaseView()->modelController()->getGridRowCount(); ++Row)
        {
            Grid->BaseView()->setFocusedRowIndex(Row); auto Index = Grid->BaseView()->focusedRow().modelIndex(0);
            while (const auto Proxy = qobject_cast<const QAbstractProxyModel*>(Index.model())) Index = Proxy->mapToSource(Index);
            if (Model->GetName(Index) == "sample.bitmap") { Selected = Index; break; }
        }
        QVERIFY(Selected.isValid()); Grid->EnsureKeyboardFocusOnView(); Panel->sig_NotifyPanelActivated();
        Timeout.restart(); Driver.start(); main_->findChild<QAction*>("hc.menu.dopus.query")->trigger(); Driver.stop();
        QTRY_VERIFY_WITH_TIMEOUT(main_->findChild<QDialog*>("hc.dataPluginDialog"), 5000); QPointer<QDialog> Info = main_->findChild<QDialog*>("hc.dataPluginDialog");
        auto* Values = Info->findChild<QTableWidget*>(); QVERIFY(Values); QTRY_COMPARE_WITH_TIMEOUT(Values->rowCount(), 6, 5000);
        QVERIFY(Values->item(0, 1)->text().contains("fixture metadata"));
        QVERIFY(Model->MatchesSearch(*Model->GetFileInfo(Selected)->storageEntry_, "fixture"));
        Info->close(); QTRY_VERIFY(Info.isNull());
        QVERIFY(legacyOpen(static_cast<CFSModel*>(Model), Selected));
        QTRY_VERIFY_WITH_TIMEOUT(main_->findChild<QDialog*>("hc.viewer.full"), 5000); QPointer<QDialog> Full = main_->findChild<QDialog*>("hc.viewer.full");
        auto* Surface = Full->findChild<QtViewerSurface*>(); QVERIFY(Surface); QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(), 5000);
        QVERIFY(Surface->Snapshot().provider_.starts_with("dopus-viewer."));
        // QWidget::grab renders Qt children only. Capture the native window so
        // the separate plugin process is included in the visual evidence.
        QTest::qWait(300);
        QVERIFY(Full->screen()->grabWindow(Full->winId()).save(QCoreApplication::applicationDirPath() + "/dopus-product.png"));
        Full->close(); QTRY_VERIFY(Full.isNull()); QTRY_VERIFY_WITH_TIMEOUT(Host->IsIdle(), 4000);
        const auto WorkspaceSaved = Panel->CaptureWorkspace(); QVERIFY(!WorkspaceSaved.tabs_[WorkspaceSaved.active_].provider_.isEmpty());
        Panel->RestoreWorkspace(WorkspaceSaved); Model = static_cast<CFSModel*>(model(Panel));
        QTRY_VERIFY_WITH_TIMEOUT(Model->GetRoot() && !Model->IsLoading() && names(Model).contains("sample.bitmap"), 10000);
        QVERIFY(Model->GetRoot()->WriteProvider());
        for (int Row = 0; Row < Model->rowCount(); ++Row) if (Model->GetName(Model->index(Row, 0)) == "sample.bitmap") QCOMPARE(Model->index(Row, 2).data().toString(), QString("VFS value 한글"));
        QMainOpts Settings(main_); Settings.show(); QCoreApplication::processEvents();
        QVERIFY(Settings.findChild<QTableWidget*>("dopus-vfsHostTable")); QVERIFY(Settings.findChild<QTableWidget*>("dopus-viewerHostTable"));
    }
    void dataPluginsUi()
    {
        using namespace hc::plugins; using namespace hc::storage; using namespace hc::operations;
        QTemporaryDir Directory; const auto Binary = Directory.filePath("data.dll");
        QVERIFY(QFile::copy(QCoreApplication::applicationDirPath() + "/hc_tc_data_fixture.dll", Binary));
        QFile File(Binary); QVERIFY(File.open(QIODevice::ReadOnly)); const auto Hash = QCryptographicHash::hash(File.readAll(), QCryptographicHash::Sha256).toHex().toStdString(); File.close();
        const auto Saved = hc::settings::AppSettings().Read(hc::settings::Section::Plugins);
        const auto Restore = qScopeGuard([&] {
            hc::settings::AppSettings().Put(hc::settings::Section::Plugins, Saved);
        });
        for (const auto Format : {PluginFormat::Wfx, PluginFormat::Wdx})
        {
            const auto Group = Format == PluginFormat::Wfx ? QString("wfx") : QString("wdx");
            PluginManifest Manifest; Manifest.id_ = "hc.test.product-data"; Manifest.version_ = "1"; Manifest.binary_ = "data.dll"; Manifest.sha256_ = Hash; Manifest.format_ = Format;
            Manifest.extensions_ = Format == PluginFormat::Wfx ? std::set{Extension::StorageRead, Extension::StorageWrite} : std::set{Extension::Metadata};
            Manifest.permissions_ = {Permission::ReadContent}; if (Format == PluginFormat::Wfx) Manifest.permissions_.insert(Permission::WriteContent);
            const auto Path = Directory.filePath(Format == PluginFormat::Wfx ? "wfx.json" : "wdx.json");
            QFile Json(Path); QVERIFY(Json.open(QIODevice::WriteOnly)); Json.write(EncodeManifest(Manifest)); Json.close();
            hc::settings::AppSettings().SetPlugins(Group, QJsonArray{QJsonObject{{"path",Binary},{"hostEnabled",true},{"hostManifest",Path},{"hostReviewed",hc::settings::JsonObject(EncodeManifest(Manifest))}}});
            QPluginHostSettings Settings(Format); Settings.Load(hc::settings::AppSettings());
            const auto Prefix = Format == PluginFormat::Wfx ? QString("wfx") : QString("wdx");
            auto* Table = Settings.findChild<QTableWidget*>(Prefix + "HostTable"); QVERIFY(Table); QCOMPARE(Table->rowCount(), 1);
            Settings.findChild<QPushButton*>(Prefix + "HostRevoke")->click();
            QVERIFY(QPluginHostSettings::DataChoices(hc::settings::AppSettings(), Format).front().options_.approval_.approved_);
            Settings.Load(hc::settings::AppSettings()); Settings.Save(hc::settings::AppSettings());
            QVERIFY(QPluginHostSettings::DataChoices(hc::settings::AppSettings(), Format).front().options_.approval_.approved_);
        }
        auto* Panel = panels_.front(); Panel->sig_NotifyPanelActivated(); const auto PreviousTab = Panel->CurrentTabIndex();
        const auto ClosePluginUi = qScopeGuard([&] {
            for (auto* Dialog : main_->findChildren<QDialog*>("hc.dataPluginDialog")) Dialog->close();
            if (Panel->CurrentTabIndex() != PreviousTab) Panel->CloseTab();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QThreadPool::globalInstance()->waitForDone();
        });
        const auto Pick = [&] {
            QTimer::singleShot(0, main_, [] {
                for (auto* Widget : QApplication::topLevelWidgets()) if (auto* Input = qobject_cast<QInputDialog*>(Widget)) Input->accept();
            });
        };
        auto* Open = main_->findChild<QAction*>("hc.menu.wfx.open"); QVERIFY(Open); Pick(); Open->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(Panel->CurrentTabIndex() != PreviousTab, 5000);
        auto* Model = static_cast<CFSModel*>(model(Panel)); QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 5000);
        QVERIFY(Model->GetRoot()->ReadLocation(Model->GetCurrentPathWithRoot()).kind_ == LocatorKind::Plugin);
        QVERIFY(names(Model).contains("hello.txt")); QVERIFY(Model->GetRoot()->WriteProvider());
        auto* Grid = qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex())); QVERIFY(Grid);
        QModelIndex Selected;
        for (int Row = 0; Row < Grid->BaseView()->modelController()->getGridRowCount(); ++Row)
        {
            Grid->BaseView()->setFocusedRowIndex(Row); auto Index = Grid->BaseView()->focusedRow().modelIndex(0);
            while (const auto Proxy = qobject_cast<const QAbstractProxyModel*>(Index.model())) Index = Proxy->mapToSource(Index);
            if (Model->GetName(Index) == "hello.txt") { Selected = Index; break; }
        }
        QVERIFY(Selected.isValid()); Grid->EnsureKeyboardFocusOnView(); Panel->sig_NotifyPanelActivated();
        auto* Bridge = TyStCommandMgr::GetInstance()->Bridge();
        auto Copy = Bridge->Dispatch("hc.file.copy", {{"destination", Directory.path()}}, "test", Selected);
        QTRY_VERIFY(Copy.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready); const auto Accepted = Copy.get(); QVERIFY(Accepted.HasValue());
        auto& Scheduler = Bridge->FileJobs()->Scheduler();
        QTRY_VERIFY_WITH_TIMEOUT(IsTerminal(Scheduler.Snapshot(Accepted.Value().jobId_).Value().state_), 5000);
        QCOMPARE(Scheduler.Snapshot(Accepted.Value().jobId_).Value().state_, JobState::Completed);
        QFile Copied(Directory.filePath("hello.txt")); QVERIFY(Copied.open(QIODevice::ReadOnly)); QCOMPARE(Copied.readAll(), QByteArray("hello from WFX")); Copied.close();
        QTRY_VERIFY(!Model->IsLoading());
        Selected = {};
        for (int Row = 0; Row < Grid->BaseView()->modelController()->getGridRowCount(); ++Row)
        {
            Grid->BaseView()->setFocusedRowIndex(Row); auto Index = Grid->BaseView()->focusedRow().modelIndex(0);
            while (const auto Proxy = qobject_cast<const QAbstractProxyModel*>(Index.model())) Index = Proxy->mapToSource(Index);
            if (Model->GetName(Index) == "hello.txt") { Selected = Index; break; }
        }
        QVERIFY(Selected.isValid()); Grid->EnsureKeyboardFocusOnView(); Panel->sig_NotifyPanelActivated();
        auto* Query = main_->findChild<QAction*>("hc.menu.wdx.query"); QVERIFY(Query); Pick(); Query->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(main_->findChild<QDialog*>("hc.dataPluginDialog"), 5000);
        QPointer<QDialog> Info = main_->findChild<QDialog*>("hc.dataPluginDialog"); auto* Values = Info->findChild<QTableWidget*>(); QVERIFY(Values);
        QTRY_COMPARE_WITH_TIMEOUT(Values->rowCount(), 10, 5000); QCOMPARE(Values->item(3, 1)->text(), QString("한글 metadata"));
        Values->cellDoubleClicked(6, 1); QTRY_COMPARE_WITH_TIMEOUT(Values->item(6, 1)->text(), QString("resolved"), 5000);
        QVERIFY(Model->MatchesSearch(*Model->GetFileInfo(Selected)->storageEntry_, "resolved"));
        QVERIFY(Info->grab().save(QCoreApplication::applicationDirPath() + "/wdx-metadata.png")); Info->close(); QTRY_VERIFY(Info.isNull());
        const auto ArchivePath = Directory.filePath("data-transfer.hcwcx");
        { QFile Archive(ArchivePath); QVERIFY(Archive.open(QIODevice::WriteOnly)); Archive.write("HCWCX01"); }
        QTemporaryDir WcxPackage; TyPlugInWCX Spec; QVERIFY(configureWcx(Spec, WcxPackage, true));
        auto Plugin = TySpWCX(new CTCPlugInWCX); QCOMPARE(Plugin->LoadWCX(Spec).ErrorCode, 0u); QVERIFY(Plugin->OpenArchive(ArchivePath).Value.value_or(false));
        auto* Other = panels_.back(); Other->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {});
        auto* ArchiveModel = static_cast<CFSModel*>(model(Other)); QTRY_VERIFY(!ArchiveModel->IsLoading()); ArchiveModel->SetProviderEnabled(true);
        ArchiveModel->AppendRoot(nsHC::TySpFileSystem(new nsHC::CFSPack(ArchivePath, Plugin))); ArchiveModel->ChangeDirectory("/"); QTRY_VERIFY(!ArchiveModel->IsLoading());
        const auto SubmitMove = [&](CFSModel* From, CFSModel* To) -> std::string {
            QModelIndex Index;
            for (int Row = 0; Row < From->rowCount(); ++Row) if (From->GetName(From->index(Row, 0)) == "hello.txt") { Index = From->index(Row, 0); break; }
            if (!Index.isValid()) return {};
            const auto Destination = Serialize(To->GetRoot()->ReadLocation(To->GetCurrentPathWithRoot()));
            auto Future = Bridge->Dispatch("hc.file.move", {{"destination", QString::fromStdString(Destination)}, {"destinationTab", To->property("hc.tabId").toString()}, {"destinationLocation", QString::fromStdString(Destination)}}, "test", Index);
            QElapsedTimer Timer; Timer.start(); while (Future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) { if (Timer.elapsed() > 5000) return {}; QTest::qWait(10); }
            const auto Result = Future.get(); if (!Result.HasValue()) { qWarning() << QString::fromStdString(Result.Failure().code_); return {}; } return Result.Value().jobId_;
        };
        auto Move = SubmitMove(Model, ArchiveModel); QVERIFY(!Move.empty());
        QTRY_VERIFY_WITH_TIMEOUT(IsTerminal(Scheduler.Snapshot(Move).Value().state_), 5000); QCOMPARE(Scheduler.Snapshot(Move).Value().state_, JobState::Completed);
        QTRY_VERIFY(!Model->IsLoading() && !ArchiveModel->IsLoading() && !names(Model).contains("hello.txt") && names(ArchiveModel).contains("hello.txt"));
        Move = SubmitMove(ArchiveModel, Model); QVERIFY(!Move.empty());
        QTRY_VERIFY_WITH_TIMEOUT(IsTerminal(Scheduler.Snapshot(Move).Value().state_), 5000); QCOMPARE(Scheduler.Snapshot(Move).Value().state_, JobState::Completed);
        QTRY_VERIFY(!Model->IsLoading() && !ArchiveModel->IsLoading() && names(Model).contains("hello.txt") && !names(ArchiveModel).contains("hello.txt"));
        Other->ChangeCurrentPath(unc_, {}); QTRY_VERIFY(!ArchiveModel->IsLoading());
        const auto WorkspaceSaved = Panel->CaptureWorkspace(); QVERIFY(!WorkspaceSaved.tabs_[WorkspaceSaved.active_].provider_.isEmpty());
        Panel->RestoreWorkspace(WorkspaceSaved); Model = static_cast<CFSModel*>(model(Panel));
        QTRY_VERIFY_WITH_TIMEOUT(Model->GetRoot() && !Model->IsLoading() && names(Model).contains("hello.txt"), 10000);
        QVERIFY(Model->GetRoot()->WriteProvider());
        Panel->CloseTab(); QTRY_VERIFY(Panel->CurrentTabIndex() == PreviousTab); QThreadPool::globalInstance()->waitForDone();
    }
    void passwordAndNestedArchivesUi()
    {
        using namespace hc::plugins; using namespace hc::storage; using namespace hc::operations;
        QTemporaryDir Directory; TyPlugInWCX Spec; QVERIFY(configureWcx(Spec, Directory, true));
        QTemporaryDir DataDirectory(QDir(unc_).filePath("hc-nested-ui-XXXXXX")); QVERIFY(DataDirectory.isValid());
        const auto Put = [&](const QString& Name, const QByteArray& Bytes) { QFile File(DataDirectory.filePath(Name)); return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size(); };
        QVERIFY(Put("encrypted.hcwcx", "HCWCX01U"));
        QVERIFY(Put("content-encrypted.hcwcx", "HCWCX01Q"));
        QByteArray Outer("HCWCX02\0", 8); const QString Inner("inner.hcwcx");
        const auto Number = [&](quint32 Value) { Outer.append(reinterpret_cast<const char*>(&Value), 4); };
        Number(2);
        for (const auto& Name : {Inner, QString("target.hcwcx")}) {
            Number(Name.size()); Number(7); Number(0); Outer.append(reinterpret_cast<const char*>(Name.utf16()), Name.size() * 2); Outer += "HCWCX01";
        }
        QVERIFY(Put("outer.hcwcx", Outer));
        const auto Original = hc::settings::AppSettings().Plugins("wcx");
        auto* Panel = panels_.front(); auto* Model = static_cast<CFSModel*>(model(Panel)); Model->SetProviderEnabled(true); Panel->sig_NotifyPanelActivated();
        auto Restore = qScopeGuard([&] {
            Panel->ChangeCurrentPath(unc_, {});
            panels_.back()->ChangeCurrentPath(unc_, {});
            hc::settings::AppSettings().SetPlugins("wcx", Original); TyStPlugInMgr::GetInstance()->Refresh();
        });
        hc::settings::AppSettings().SetPlugins("wcx", QJsonArray{QJsonObject{{"path",Spec.FileFullPath},{"extensions",QJsonArray{"hcwcx"}},{"hostEnabled",true},
            {"hostManifest",Spec.hostManifest_},{"hostReviewed",hc::settings::JsonObject(EncodeManifest(Spec.hostApproval_.reviewed_))}}});
        TyStPlugInMgr::GetInstance()->Refresh();
        const auto Index = [&](const QString& Name) { for (int Row = 0; Row < Model->rowCount(); ++Row) if (Model->GetName(Model->index(Row, 0)) == Name) return Model->index(Row, 0); return QModelIndex{}; };
        Panel->ChangeCurrentPath(QDir::toNativeSeparators(DataDirectory.path()), {}); QTRY_VERIFY(!Model->IsLoading());
        unsigned Prompts = 0; bool Cancel = false; QTimer Input;
        connect(&Input, &QTimer::timeout, this, [&] {
            for (auto* Widget : QApplication::topLevelWidgets()) if (auto* Dialog = qobject_cast<QDialog*>(Widget); Dialog && Dialog->isVisible() && Dialog->objectName() == "hc.archive.password") {
                auto* Field = Dialog->findChild<QLineEdit*>("hc.archive.passwordValue"); QVERIFY(Field); QCOMPARE(Field->echoMode(), QLineEdit::Password); ++Prompts;
                if (Cancel) Dialog->reject(); else { Field->setText(Prompts == 1 ? "wrong" : "비밀-pass"); Dialog->accept(); }
            }
        }); Input.start(30);
        QVERIFY(Index("encrypted.hcwcx").isValid()); Model->ChangeDirectory(Index("encrypted.hcwcx"));
        QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading() && names(Model).contains("alpha.txt"), 10000); QCOMPARE(Prompts, 2u);
        Model->ChangeDirectory(".."); QTRY_VERIFY(!Model->IsLoading() && names(Model).contains("encrypted.hcwcx"));
        hc::passwords::ForgetSessionPasswords(); Cancel = true; Model->ChangeDirectory(Index("encrypted.hcwcx")); QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 10000);
        QVERIFY(Model->LastNavigationError()); QCOMPARE(Model->LastNavigationError()->category_, ErrorCategory::Cancelled); Input.stop();
        {
            Panel->ChangeCurrentPath(QDir::toNativeSeparators(DataDirectory.path()), {}); QTRY_VERIFY(!Model->IsLoading());
            Model->ChangeDirectory(Index("content-encrypted.hcwcx")); QTRY_VERIFY(!Model->IsLoading() && names(Model).contains("alpha.txt"));
            auto* Bridge = TyStCommandMgr::GetInstance()->Bridge();
            auto Future = Bridge->Dispatch("hc.file.copy", {{"destination", DataDirectory.path()}}, "test", Index("alpha.txt"));
            QTRY_VERIFY(Future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
            auto Accepted = Future.get(); QVERIFY2(Accepted.HasValue(), Accepted.HasValue() ? "" : Accepted.Failure().code_.c_str());
            auto& Scheduler = Bridge->FileJobs()->Scheduler(); const auto Job = Accepted.Value().jobId_;
            QTRY_COMPARE_WITH_TIMEOUT(Scheduler.Snapshot(Job).Value().state_, JobState::WaitingForUser, 10000);
            QCOMPARE(Scheduler.Snapshot(Job).Value().interaction_->kind_, InteractionKind::Password);
            auto* Field = main_->findChild<QLineEdit*>("hc.jobs.newName"); QVERIFY(Field);
            auto* Confirm = main_->findChild<QPushButton*>("hc.jobs.choice." + QString::number(int(InteractionChoice::Confirm))); QVERIFY(Confirm);
            QTRY_VERIFY(!Field->isVisible() && Confirm->isVisible()); QCOMPARE(Field->echoMode(), QLineEdit::Password);
            Cancel = false; Input.start(30); QTest::mouseClick(Confirm, Qt::LeftButton); Input.stop();
            QTRY_VERIFY_WITH_TIMEOUT(IsTerminal(Scheduler.Snapshot(Job).Value().state_), 10000);
            QCOMPARE(Scheduler.Snapshot(Job).Value().state_, JobState::Completed);
            QFile Extracted(DataDirectory.filePath("alpha.txt")); QVERIFY(Extracted.open(QIODevice::ReadOnly)); QCOMPARE(Extracted.readAll(), QByteArray("alpha"));
            QTRY_VERIFY(Field->text().isEmpty());
        }
        Panel->ChangeCurrentPath(QDir::toNativeSeparators(DataDirectory.path()), {}); QTRY_VERIFY(!Model->IsLoading());
        Model->ChangeDirectory(Index("outer.hcwcx")); QTRY_VERIFY(!Model->IsLoading() && names(Model).contains(Inner));
        Model->ChangeDirectory(Index(Inner)); QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading() && names(Model).contains("alpha.txt"), 10000);
        QCOMPARE(Model->WorkspaceLocation().nesting_.size(), 2);
        auto* Bridge = TyStCommandMgr::GetInstance()->Bridge();
        auto Future = Bridge->Dispatch("hc.file.rename", {{"destination", "renamed.txt"}}, "test", Index("alpha.txt"));
        QTRY_VERIFY(Future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready); auto Accepted = Future.get();
        QVERIFY2(Accepted.HasValue(), Accepted.HasValue() ? "" : Accepted.Failure().code_.c_str());
        auto& Scheduler = Bridge->FileJobs()->Scheduler(); const auto Job = Accepted.Value().jobId_;
        QTRY_VERIFY_WITH_TIMEOUT(Scheduler.Snapshot(Job).HasValue() && IsTerminal(Scheduler.Snapshot(Job).Value().state_), 20000);
        QCOMPARE(Scheduler.Snapshot(Job).Value().state_, JobState::Completed);
        QTRY_VERIFY(!Model->IsLoading() && names(Model).contains("renamed.txt")); QVERIFY(!names(Model).contains("alpha.txt"));
        hc::workspace::TabState Saved; Saved.location_ = Model->WorkspaceLocation(); Saved.provider_ = Spec.hostManifest_;
        RestoreWorkspaceLocation(Model, Saved); QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading() && names(Model).contains("renamed.txt"), 10000);
        QCOMPARE(Model->WorkspaceLocation().nesting_.size(), 2);
        auto* Target = static_cast<CFSModel*>(model(panels_.back())); Target->SetProviderEnabled(true);
        auto Destination = Saved; Destination.location_.nesting_.front().entry_ = "/target.hcwcx";
        RestoreWorkspaceLocation(Target, Destination); QTRY_VERIFY_WITH_TIMEOUT(!Target->IsLoading() && names(Target).contains("alpha.txt"), 10000);
        Panel->sig_NotifyPanelActivated();
        Future = Bridge->Dispatch("hc.file.move", {{"destination", QString::fromStdString(Serialize(Destination.location_))},
            {"destinationTab", Target->property("hc.tabId").toString()}, {"destinationLocation", QString::fromStdString(Serialize(Destination.location_))}}, "test", Index("renamed.txt"));
        QTRY_VERIFY(Future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready); Accepted = Future.get();
        QVERIFY2(Accepted.HasValue(), Accepted.HasValue() ? "" : Accepted.Failure().code_.c_str());
        const auto MoveJob = Accepted.Value().jobId_;
        QTRY_VERIFY_WITH_TIMEOUT(Scheduler.Snapshot(MoveJob).HasValue() && IsTerminal(Scheduler.Snapshot(MoveJob).Value().state_), 30000);
        QCOMPARE(Scheduler.Snapshot(MoveJob).Value().state_, JobState::Completed);
        QTRY_VERIFY(!Model->IsLoading() && !Target->IsLoading() && !names(Model).contains("renamed.txt") && names(Target).contains("renamed.txt"));
        RestoreWorkspaceLocation(Target, Destination); QTRY_VERIFY(!Target->IsLoading() && names(Target).contains("renamed.txt"));
        panels_.back()->ChangeCurrentPath(unc_, {}); QTRY_VERIFY(!Target->IsLoading());
        Model->ChangeDirectory(".."); QTRY_VERIFY(!Model->IsLoading() && names(Model).contains(Inner));
        Model->ChangeDirectory(".."); QTRY_VERIFY(!Model->IsLoading() && names(Model).contains("outer.hcwcx"));
        QThreadPool::globalInstance()->waitForDone();
    }
    void archiveCommandsAndBatchUi_data()
    { QTest::addColumn<bool>("Hosted"); QTest::newRow("legacy") << false; QTest::newRow("hosted") << true; }
    void archiveCommandsAndBatchUi()
    {
        QFETCH(bool, Hosted);
        using namespace hc::storage;
        using namespace hc::operations;
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        const auto ArchivePath = Directory.filePath("jobs.hcwcx");
        { QFile File(ArchivePath); QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write("HCWCX01"), 7); }
        const auto SourcePath = Directory.filePath("source"); QVERIFY(QDir().mkdir(SourcePath));
        { QFile File(SourcePath + "/new.txt"); QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write("archive input"), 13); }
        auto* Left = static_cast<CFSModel*>(model(panels_.front()));
        auto* Right = static_cast<CFSModel*>(model(panels_.back()));
        Left->SetProviderEnabled(true); Right->SetProviderEnabled(true);
        panels_.front()->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {});
        panels_.back()->ChangeCurrentPath(QDir::toNativeSeparators(SourcePath), {});
        QTRY_VERIFY(!Left->IsLoading() && !Right->IsLoading());
        TyPlugInWCX Spec; QVERIFY(configureWcx(Spec, Directory, Hosted));
        auto Plugin = TySpWCX(new CTCPlugInWCX); QCOMPARE(Plugin->LoadWCX(Spec).ErrorCode, 0u);
        QVERIFY(Plugin->OpenArchive(ArchivePath).Value.value_or(false));
        auto Pack = nsHC::TySpFileSystem(new nsHC::CFSPack(ArchivePath, Plugin));
        Left->AppendRoot(Pack); Left->ChangeDirectory("/"); QTRY_VERIFY(!Left->IsLoading());
        auto* Bridge = TyStCommandMgr::GetInstance()->Bridge(); auto& Scheduler = Bridge->FileJobs()->Scheduler();
        const auto Index = [](CFSModel* Model, const QString& Name) {
            for (int Row = 0; Row < Model->rowCount(); ++Row) if (Model->GetName(Model->index(Row, 0)) == Name) return Model->index(Row, 0);
            return QModelIndex{};
        };
        const auto Submit = [&](const QString& Id, QVariantMap Parameters, CFSModel* Model, const QString& Name) -> std::string {
            auto Future = Bridge->Dispatch(Id, Parameters, "test", Index(Model, Name));
            QElapsedTimer Timer; Timer.start();
            while (Future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
            {
                if (Timer.elapsed() >= 5000) return {};
                QTest::qWait(10);
            }
            const auto Accepted = Future.get(); return Accepted.HasValue() ? Accepted.Value().jobId_ : std::string{};
        };
        const auto Finished = [&](const std::string& Id) { const auto State = Scheduler.Snapshot(Id); return State.HasValue() && IsTerminal(State.Value().state_); };
        auto Job = Submit("hc.file.mkdir", {{"destination", "/target"}}, Left, "alpha.txt"); QVERIFY(!Job.empty());
        QTRY_VERIFY(Finished(Job)); QVERIFY2(Scheduler.Snapshot(Job).Value().state_ == JobState::Completed, Scheduler.Snapshot(Job).Value().error_ ? Scheduler.Snapshot(Job).Value().error_->code_.c_str() : "archive operation failed");
        QTRY_VERIFY(!Left->IsLoading() && names(Left).contains("target"));
        Job = Submit("hc.file.rename", {{"destination", "renamed.txt"}}, Left, "alpha.txt"); QVERIFY(!Job.empty());
        QTRY_VERIFY(Finished(Job)); QCOMPARE(Scheduler.Snapshot(Job).Value().state_, JobState::Completed);
        QTRY_VERIFY(!Left->IsLoading() && names(Left).contains("renamed.txt"));
        Locator Target{LocatorKind::Archive, ArchivePath.toStdString(), {{"wcx", "/target"}}};
        Job = Submit("hc.file.move", {{"destination", QString::fromStdString(Serialize(Target))}}, Left, "renamed.txt"); QVERIFY(!Job.empty());
        QTRY_VERIFY(Finished(Job)); QCOMPARE(Scheduler.Snapshot(Job).Value().state_, JobState::Completed);
        QTRY_VERIFY(!Left->IsLoading() && !names(Left).contains("renamed.txt"));
        QFile Before(ArchivePath); QVERIFY(Before.open(QIODevice::ReadOnly)); const auto Original = Before.readAll(); Before.close();
        Job = Submit("hc.file.delete", {}, Left, "target"); QVERIFY(!Job.empty());
        QTRY_COMPARE(Scheduler.Snapshot(Job).Value().state_, JobState::WaitingForUser);
        auto* Cancel = main_->findChild<QPushButton*>("hc.jobs.cancel"); QVERIFY(Cancel); QTest::mouseClick(Cancel, Qt::LeftButton);
        QTRY_VERIFY(Finished(Job)); QCOMPARE(Scheduler.Snapshot(Job).Value().state_, JobState::Cancelled);
        QVERIFY(Before.open(QIODevice::ReadOnly)); QCOMPARE(Before.readAll(), Original); Before.close();
        QTest::qWait(150); QTRY_VERIFY(!Left->IsLoading());
        Job = Submit("hc.file.delete", {}, Left, "target"); QVERIFY(!Job.empty());
        QTRY_COMPARE(Scheduler.Snapshot(Job).Value().state_, JobState::WaitingForUser);
        auto* Confirm = main_->findChild<QPushButton*>("hc.jobs.choice." + QString::number(static_cast<int>(InteractionChoice::Confirm))); QVERIFY(Confirm);
        QTRY_VERIFY(Confirm->isVisible()); QTest::mouseClick(Confirm, Qt::LeftButton);
        QTRY_VERIFY(Finished(Job)); QVERIFY2(Scheduler.Snapshot(Job).Value().state_ == JobState::Completed, Scheduler.Snapshot(Job).Value().error_ ? Scheduler.Snapshot(Job).Value().error_->code_.c_str() : "archive delete failed");
        QTRY_VERIFY(!Left->IsLoading() && !names(Left).contains("target"));
        Target.nesting_[0].entry_ = "/";
        const auto Parameters = [&] { return QVariantMap{{"destination", QString::fromStdString(Serialize(Target))},
            {"destinationTab", Left->property("hc.tabId").toString()}, {"destinationLocation", QString::fromStdString(Serialize(Target))}}; };
        auto QueuedParameters = Parameters(); QueuedParameters["queued"] = true;
        QVERIFY(Before.open(QIODevice::ReadOnly)); const auto BeforeQueuedPack = Before.readAll(); Before.close();
        Job = Submit("hc.file.copy", QueuedParameters, Right, "new.txt"); QVERIFY(!Job.empty());
        QTest::qWait(100); QVERIFY(Scheduler.Snapshot(Job).Value().deferred_);
        QVERIFY(Before.open(QIODevice::ReadOnly)); QCOMPARE(Before.readAll(), BeforeQueuedPack); Before.close();
        QVERIFY(Scheduler.Control({Job}, JobControl::Start).HasValue());
        QTRY_VERIFY(Finished(Job)); QCOMPARE(Scheduler.Snapshot(Job).Value().state_, JobState::Completed);
        QTRY_VERIFY(!Left->IsLoading() && !Right->IsLoading() && names(Left).contains("new.txt"));
        QVERIFY(QDir().mkdir(Directory.filePath("extracted")));
        const auto Extract = Submit("hc.file.copy", {{"destination", Directory.filePath("extracted")}, {"queued", true}}, Left, "new.txt"); QVERIFY(!Extract.empty());
        QTest::qWait(100); QVERIFY(Scheduler.Snapshot(Extract).Value().deferred_); QVERIFY(!QFile::exists(Directory.filePath("extracted/new.txt")));
        QVERIFY(Scheduler.Control({Extract}, JobControl::Start).HasValue()); QTRY_VERIFY(Finished(Extract)); QCOMPARE(Scheduler.Snapshot(Extract).Value().state_, JobState::Completed);
        QFile Extracted(Directory.filePath("extracted/new.txt")); QVERIFY(Extracted.open(QIODevice::ReadOnly)); QCOMPARE(Extracted.readAll(), QByteArray("archive input")); Extracted.close();
        QTRY_VERIFY(!Left->IsLoading() && !Right->IsLoading());
        Job = Submit("hc.file.copy", Parameters(), Right, "new.txt"); QVERIFY(!Job.empty());
        QTRY_COMPARE(Scheduler.Snapshot(Job).Value().state_, JobState::WaitingForUser);
        auto* NewName = main_->findChild<QLineEdit*>("hc.jobs.newName"); QVERIFY(NewName); NewName->setText("copy.txt");
        auto* Rename = main_->findChild<QPushButton*>("hc.jobs.choice." + QString::number(static_cast<int>(InteractionChoice::Rename))); QVERIFY(Rename);
        QTRY_VERIFY(Rename->isVisible()); QTest::mouseClick(Rename, Qt::LeftButton);
        QTRY_VERIFY(Finished(Job)); QCOMPARE(Scheduler.Snapshot(Job).Value().state_, JobState::Completed);
        QTRY_VERIFY(!Left->IsLoading() && !Right->IsLoading() && names(Left).contains("copy.txt"));
        Left->ChangeDirectory("sub"); QTRY_VERIFY(!Left->IsLoading());
        auto Stale = Bridge->Dispatch("hc.file.copy", Parameters(), "test", Index(Right, "new.txt"));
        QTRY_VERIFY(Stale.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        auto Rejected = Stale.get(); QVERIFY(!Rejected.HasValue()); QCOMPARE(Rejected.Failure().code_, "archive-destination-location-changed");
        Left->ChangeDirectory(".."); QTRY_VERIFY(!Left->IsLoading());
        Right->AppendRoot(Pack); Right->ChangeDirectory("/"); QTRY_VERIFY(!Right->IsLoading());
        Right->Refresh(); QTRY_VERIFY(!Right->IsLoading());
        QVERIFY(Left->SnapshotProvider() != Right->SnapshotProvider());
        Job = Submit("hc.file.rename", {{"destination", "copy2.txt"}}, Left, "copy.txt"); QVERIFY(!Job.empty());
        QTRY_VERIFY(Finished(Job)); QCOMPARE(Scheduler.Snapshot(Job).Value().state_, JobState::Completed);
        QTRY_VERIFY(!Left->IsLoading() && !Right->IsLoading() && names(Left).contains("copy2.txt") && names(Right).contains("copy2.txt"));
        const QVariantMap MoveParameters{{"destination", QString::fromStdString(Serialize(Target))},
            {"destinationTab", Right->property("hc.tabId").toString()}, {"destinationLocation", QString::fromStdString(Serialize(Target))}};
        Right->ChangeDirectory("sub"); QTRY_VERIFY(!Right->IsLoading());
        auto StaleMove = Bridge->Dispatch("hc.file.move", MoveParameters, "test", Index(Left, "new.txt"));
        QTRY_VERIFY(StaleMove.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        Rejected = StaleMove.get(); QVERIFY(!Rejected.HasValue()); QCOMPARE(Rejected.Failure().code_, "archive-destination-location-changed");
        panels_.back()->ChangeCurrentPath(unc_, {}); QTRY_VERIFY(!Right->IsLoading());
        if (!Hosted) Left->SetProviderEnabled(false);
        QTRY_VERIFY(!Left->IsLoading()); QCOMPARE(names(Left), QStringList({"copy2.txt", "new.txt", "sub"}));
        Left->Refresh(); QTRY_VERIFY(!Left->IsLoading()); QCOMPARE(names(Left), QStringList({"copy2.txt", "new.txt", "sub"}));
        QVERIFY(QFile::remove(ArchivePath)); Left->Refresh(); QTRY_VERIFY(!Left->IsLoading());
        if (Hosted)
        {
            // Failed asynchronous navigation keeps the last accepted snapshot and reports the error.
            QVERIFY(Left->LastNavigationError().has_value());
            QCOMPARE(Left->LastNavigationError()->category_, ErrorCategory::NotFound);
            QCOMPARE(names(Left), QStringList({"copy2.txt", "new.txt", "sub"}));
        }
        else QVERIFY(names(Left).isEmpty());
        Left->SetProviderEnabled(true); QTRY_VERIFY(!Left->IsLoading());
        panels_.front()->ChangeCurrentPath(unc_, {}); panels_.back()->ChangeCurrentPath(unc_, {});
        QTRY_VERIFY(!Left->IsLoading() && !Right->IsLoading());
    }

    void archiveCrossContainerMovesUi_data()
    { archiveCommandsAndBatchUi_data(); }
    void archiveCrossContainerMovesUi()
    {
        QFETCH(bool, Hosted);
        using namespace hc::storage; using namespace hc::operations;
        QTemporaryDir Directory(QDir(unc_).filePath("hc-move-ui-XXXXXX")); QVERIFY(Directory.isValid());
        QTemporaryDir PluginDirectory; QVERIFY(PluginDirectory.isValid());
        const auto Native = QDir::toNativeSeparators(Directory.filePath("native")); QVERIFY(QDir().mkdir(Native));
        { QFile File(Native + "/move.txt"); QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write("move-content"), 12); }
        const auto A = Directory.filePath("a.hcwcx"), B = Directory.filePath("b.hcwcx");
        for (const auto& Path : {A, B}) { QFile File(Path); QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write("HCWCX01"), 7); }
        auto* Left = static_cast<CFSModel*>(model(panels_.front())); auto* Right = static_cast<CFSModel*>(model(panels_.back()));
        Left->SetProviderEnabled(true); Right->SetProviderEnabled(true);
        panels_.front()->ChangeCurrentPath(Native, {}); panels_.back()->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {});
        QTRY_VERIFY(!Left->IsLoading() && !Right->IsLoading());
        TyPlugInWCX Spec; QVERIFY(configureWcx(Spec, PluginDirectory, Hosted));
        const auto Mount = [&](CFSModel* Model, const QString& Path) -> bool {
            auto Plugin = TySpWCX(new CTCPlugInWCX);
            if (Plugin->LoadWCX(Spec).ErrorCode || !Plugin->OpenArchive(Path).Value.value_or(false)) return false;
            Model->AppendRoot(nsHC::TySpFileSystem(new nsHC::CFSPack(Path, Plugin))); Model->ChangeDirectory("/"); return true;
        };
        QVERIFY(Mount(Right, A)); QTRY_VERIFY(!Right->IsLoading());
        auto* Bridge = TyStCommandMgr::GetInstance()->Bridge(); auto& Scheduler = Bridge->FileJobs()->Scheduler();
        const auto Move = [&](CFSModel* Source, const QString& Destination) -> std::string {
            QModelIndex Cursor;
            for (int Row = 0; Row < Source->rowCount(); ++Row) if (Source->GetName(Source->index(Row, 0)) == "move.txt") { Cursor = Source->index(Row, 0); break; }
            if (!Cursor.isValid()) return {};
            auto Future = Bridge->Dispatch("hc.file.move", {{"destination", Destination}}, "test", Cursor);
            QElapsedTimer Timer; Timer.start();
            while (Future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) { if (Timer.elapsed() > 5000) return {}; QTest::qWait(10); }
            auto Accepted = Future.get(); return Accepted.HasValue() ? Accepted.Value().jobId_ : std::string{};
        };
        const auto Finished = [&](const std::string& Id) { const auto State = Scheduler.Snapshot(Id); return State.HasValue() && IsTerminal(State.Value().state_); };
        auto Job = Move(Left, QString::fromStdString(Serialize({LocatorKind::Archive, A.toStdString(), {{"wcx", "/"}}}))); QVERIFY(!Job.empty());
        QTRY_VERIFY(Finished(Job)); QCOMPARE(Scheduler.Snapshot(Job).Value().state_, JobState::Completed);
        QTRY_VERIFY(!Left->IsLoading() && !Right->IsLoading() && names(Right).contains("move.txt")); QVERIFY(!QFile::exists(Native + "/move.txt"));
        panels_.front()->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {}); QTRY_VERIFY(!Left->IsLoading());
        QVERIFY(Mount(Left, B)); QTRY_VERIFY(!Left->IsLoading());
        Job = Move(Right, QString::fromStdString(Serialize({LocatorKind::Archive, B.toStdString(), {{"wcx", "/"}}}))); QVERIFY(!Job.empty());
        QTRY_VERIFY(Finished(Job)); QCOMPARE(Scheduler.Snapshot(Job).Value().state_, JobState::Completed);
        QTRY_VERIFY(!Left->IsLoading() && !Right->IsLoading() && names(Left).contains("move.txt") && !names(Right).contains("move.txt"));
        panels_.back()->ChangeCurrentPath(Native, {}); QTRY_VERIFY(!Right->IsLoading());
        Job = Move(Left, Native); QVERIFY(!Job.empty());
        QTRY_VERIFY(Finished(Job)); QCOMPARE(Scheduler.Snapshot(Job).Value().state_, JobState::Completed);
        QTRY_VERIFY(!Left->IsLoading() && !Right->IsLoading() && !names(Left).contains("move.txt") && names(Right).contains("move.txt"));
        QFile File(Native + "/move.txt"); QVERIFY(File.open(QIODevice::ReadOnly)); QCOMPARE(File.readAll(), "move-content"); File.close();
        panels_.front()->ChangeCurrentPath(unc_, {}); panels_.back()->ChangeCurrentPath(unc_, {});
        QTRY_VERIFY(!Left->IsLoading() && !Right->IsLoading());
    }

    void commandEntryPointsAndTabLifetime()
    {
        using namespace hc::commands;
        using namespace hc::storage;
        auto* Bridge = TyStCommandMgr::GetInstance()->Bridge();
        QVERIFY(Bridge);
        std::vector<EventEnvelope> Events;
        auto Subscription = Bridge->Events()->Subscribe("", MakeQtExecutor(this), [&](const auto& Event) { Events.push_back(Event); });
        QVERIFY(Subscription.HasValue());
        for (const auto& Source : {QStringLiteral("menu"), QStringLiteral("toolbar"), QStringLiteral("shortcut")})
        {
            Events.clear();
            if (Source == "shortcut")
            {
                QKeyEvent Key(QEvent::KeyPress, Qt::Key_F5, Qt::NoModifier);
                QVERIFY(TyStCommandMgr::GetInstance()->ProcessKeyPressEvent(&Key, {}));
            }
            else
            {
                auto* Action = main_->findChild<QAction*>("hc." + Source + ".hc.navigate.refresh");
                QVERIFY(Action);
                Action->trigger();
            }
            const auto HasEvent = [&](const std::string& Id) {
                return std::any_of(Events.begin(), Events.end(), [&](const auto& Event) { return Event.id_ == Id; });
            };
            QTRY_VERIFY_WITH_TIMEOUT(HasEvent("navigation.completed"), 15000);
            const auto Started = std::find_if(Events.begin(), Events.end(), [](const auto& Event) { return Event.id_ == "command.started"; });
            QVERIFY(Started != Events.end());
            QCOMPARE(std::get<std::string>(Started->payload_.at("command")), std::string("hc.navigate.refresh"));
            QCOMPARE(std::get<std::string>(Started->payload_.at("source")), Source.toStdString());
            const auto Complete = std::find_if(Events.begin(), Events.end(), [](const auto& Event) { return Event.id_ == "navigation.completed"; });
            QCOMPARE(Complete->correlationId_, Started->correlationId_);
            QVERIFY(!Started->correlationId_.empty());
        }
        auto* Panel = panels_.front();
        auto* Model = static_cast<CFSModel*>(model(Panel));
        auto Path = Bridge->Dispatch("hc.navigate.path", {{"path", unc_}}, "test", Model->index(0, 0));
        QTRY_VERIFY(Path.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        QVERIFY(Path.get().HasValue());
        QTRY_VERIFY(!Model->IsLoading());
        QCOMPARE(Model->GetCurrentPathWithRoot(), unc_);
        auto Add = Bridge->Dispatch("hc.tab.new", {}, "test", Model->index(0, 0));
        QTRY_VERIFY(Add.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        QVERIFY(Add.get().HasValue());
        auto* NewModel = static_cast<CFSModel*>(model(Panel));
        QVERIFY(NewModel != Model);
        QTRY_VERIFY(!NewModel->IsLoading());
        CommandInvocation Closed;
        Closed.id_ = "hc.navigate.refresh";
        Closed.permissions_.insert("navigation");
        Closed.context_ = Bridge->Capture(NewModel->index(0, 0));
        QVERIFY(!Closed.context_.tabId_.empty());
        QPointer<CFSModel> Guard(NewModel);
        Panel->CloseTab();
        QTRY_VERIFY(Guard.isNull());
        auto Rejected = Bridge->DispatchInvocation(Closed);
        QTRY_VERIFY(Rejected.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        QCOMPARE(Rejected.get().Failure().code_, std::string("tab-closed"));
        QCOMPARE(model(Panel), Model);
        QVERIFY(main_->grab().save(QCoreApplication::applicationDirPath() + "/m2-navigation.png"));
    }

    void queuedQtOwnerDestruction()
    {
        using namespace hc::commands;
        using namespace hc::storage;
        auto Registry = std::make_shared<CommandRegistry>();
        auto Hub = std::make_shared<EventHub>();
        auto* Owner = new QObject;
        bool Executed = false;
        QVERIFY(Registry->Register({"test"}, {{}, [&](const auto&) -> Result<CommandOutcome> { Executed = true; return CommandOutcome{}; }}, MakeQtExecutor(Owner)).HasValue());
        CommandDispatcher Dispatcher(Registry, Hub);
        CommandInvocation Invocation;
        Invocation.id_ = "test";
        Invocation.permissions_.insert("navigation");
        auto Pending = Dispatcher.Dispatch(Invocation);
        delete Owner;
        QTRY_VERIFY(Pending.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        QCOMPARE(Pending.get().Failure().category_, ErrorCategory::Cancelled);
        QVERIFY(!Executed);
    }

    void commandCancellationCorrelation()
    {
        using namespace hc::commands;
        auto* Bridge = TyStCommandMgr::GetInstance()->Bridge();
        auto* Model = static_cast<CFSModel*>(model(panels_.front()));
        std::vector<EventEnvelope> Events;
        auto Subscription = Bridge->Events()->Subscribe("", MakeQtExecutor(this), [&](const auto& Event) { Events.push_back(Event); });
        auto Provider = std::make_shared<GatedProvider>();
        auto Release = qScopeGuard([&] { Provider->release_.set_value(); });
        Model->SetRoot(nsHC::TySpFileSystem(new GatedFs(Provider)));
        Model->SetCurrentPath("\\");
        auto Started = Bridge->Dispatch("hc.navigate.refresh", {}, "test", Model->index(0, 0));
        QTRY_VERIFY(Started.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        auto Outcome = Started.get();
        QVERIFY(Outcome.HasValue());
        QCOMPARE(Outcome.Value().status_, CommandOutcome::Status::Accepted);
        QTRY_VERIFY(Provider->started_.load());
        auto Invalid = Bridge->Dispatch("hc.navigate.path", {{"path", QStringLiteral("relative")}}, "test", Model->index(0, 0));
        QTRY_VERIFY(Invalid.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        QVERIFY(!Invalid.get().HasValue());
        QVERIFY(Model->IsLoading());
        auto Cancelled = Bridge->Dispatch("hc.navigate.cancel", {}, "test", Model->index(0, 0));
        QTRY_VERIFY(Cancelled.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        QVERIFY(Cancelled.get().HasValue());
        QVERIFY(!Model->IsLoading());
        const auto HasTerminal = [&] {
            return std::any_of(Events.begin(), Events.end(), [&](const auto& Event) {
                return Event.id_ == "navigation.failed" && Event.correlationId_ == Outcome.Value().jobId_;
            });
        };
        QTRY_VERIFY(HasTerminal());
        QCOMPARE(Model->GetCurrentPathWithRoot(), unc_);
        QVERIFY(Model->property("hc.correlation").toString().isEmpty());
    }

    void fileCommandsAndProgressUi()
    {
        using namespace hc::operations;
        using namespace hc::commands;
        using namespace hc::storage;
        QTemporaryDir Directory;
        QVERIFY(Directory.isValid());
        const auto Source = Directory.filePath("source");
        const auto Destination = Directory.filePath("destination");
        QVERIFY(QDir().mkpath(Source)); QVERIFY(QDir().mkpath(Destination));
        for (const auto& Name : {QStringLiteral("first.txt"), QStringLiteral("second.txt")})
        { QFile File(Source + '/' + Name); QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write(Name.toUtf8()), Name.size()); }
        auto* Panel = panels_.front();
        auto* Model = static_cast<CFSModel*>(model(Panel));
        Model->SetProviderEnabled(true);
        Panel->ChangeCurrentPath(QDir::toNativeSeparators(Source), {});
        QTRY_VERIFY(!Model->IsLoading());
        auto* View = qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex()));
        auto* Bridge = TyStCommandMgr::GetInstance()->Bridge();
        auto& Scheduler = Bridge->FileJobs()->Scheduler();
        const auto IndexFor = [&](const QString& Name) {
            for (int Row = 0; Row < Model->rowCount(); ++Row)
                if (Model->GetName(Model->index(Row, 0)) == Name) return Model->index(Row, 0);
            return QModelIndex{};
        };
        for (const auto& Name : {QStringLiteral("first.txt"), QStringLiteral("second.txt")})
        {
            const auto Proxy = qobject_cast<QAbstractProxyModel*>(View->BaseView()->model());
            const auto Row = View->BaseView()->getRow(Proxy ? Proxy->mapFromSource(IndexFor(Name)) : IndexFor(Name));
            QVERIFY(Row.isValid()); View->BaseView()->selectRow(Row.rowIndex(), Qtitan::Select);
        }
        QCOMPARE(View->BaseView()->selection()->selectedRowIndexes().size(), 2);
        auto Copy = Bridge->Dispatch("hc.file.copy", {{"destination", Destination}}, "test", IndexFor("first.txt"));
        QTRY_VERIFY(Copy.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        auto Accepted = Copy.get(); QVERIFY(Accepted.HasValue());
        QCOMPARE(Accepted.Value().status_, CommandOutcome::Status::Accepted);
        const auto Copied = Accepted.Value().jobId_;
        QTRY_VERIFY(IsTerminal(Scheduler.Snapshot(Copied).Value().state_));
        QCOMPARE(Scheduler.Snapshot(Copied).Value().state_, JobState::Completed);
        QCOMPARE(Scheduler.Snapshot(Copied).Value().completedItems_, 2u);
        QVERIFY(QFile::exists(Destination + "/first.txt")); QVERIFY(QFile::exists(Destination + "/second.txt"));
        QTest::qWait(150); QTRY_VERIFY(!Model->IsLoading());

        // Captured context is rejected after navigation; permission checks run first.
        CommandInvocation Stale;
        Stale.id_ = "hc.file.delete"; Stale.context_ = Bridge->Capture(IndexFor("first.txt"));
        auto Denied = Bridge->DispatchInvocation(Stale);
        QCOMPARE(Denied.get().Failure().category_, ErrorCategory::AccessDenied);
        Stale.permissions_.insert("file-write");
        Panel->ChangeCurrentPath(QDir::toNativeSeparators(Destination), {});
        QTRY_VERIFY(!Model->IsLoading());
        auto Rejected = Bridge->DispatchInvocation(Stale);
        QTRY_VERIFY(Rejected.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        QCOMPARE(Rejected.get().Failure().code_, std::string("navigation-context-changed"));
        QVERIFY(QFile::exists(Source + "/first.txt"));

        auto Rename = Bridge->Dispatch("hc.file.rename", {{"destination", Destination + "/renamed.txt"}}, "test", IndexFor("first.txt"));
        QTRY_VERIFY(Rename.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        const auto Renamed = Rename.get(); QVERIFY(Renamed.HasValue());
        QTRY_COMPARE(Scheduler.Snapshot(Renamed.Value().jobId_).Value().state_, JobState::Completed);
        QTest::qWait(150); QTRY_VERIFY(!Model->IsLoading());
        QVERIFY(QFile::exists(Destination + "/renamed.txt"));
        auto Delete = Bridge->Dispatch("hc.file.delete", {}, "test", IndexFor("renamed.txt"));
        QTRY_VERIFY(Delete.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        const auto Deleted = Delete.get(); QVERIFY(Deleted.HasValue());
        QTRY_COMPARE(Scheduler.Snapshot(Deleted.Value().jobId_).Value().state_, JobState::WaitingForUser);
        QVERIFY(QFile::exists(Destination + "/renamed.txt"));
        auto* Confirm = main_->findChild<QPushButton*>("hc.jobs.choice." + QString::number(static_cast<int>(InteractionChoice::Confirm)));
        QVERIFY(Confirm); QTRY_VERIFY(Confirm->isVisible());
        QTest::mouseClick(Confirm, Qt::LeftButton);
        QTRY_COMPARE(Scheduler.Snapshot(Deleted.Value().jobId_).Value().state_, JobState::Completed);
        QVERIFY(!QFile::exists(Destination + "/renamed.txt"));
        QTest::qWait(150); QTRY_VERIFY(!Model->IsLoading());

        auto Cancel = Bridge->Dispatch("hc.file.delete", {}, "test", IndexFor("second.txt"));
        QTRY_VERIFY(Cancel.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        const auto Cancelled = Cancel.get(); QVERIFY(Cancelled.HasValue());
        QTRY_COMPARE(Scheduler.Snapshot(Cancelled.Value().jobId_).Value().state_, JobState::WaitingForUser);
        auto* CancelButton = main_->findChild<QPushButton*>("hc.jobs.cancel");
        QVERIFY(CancelButton); QTest::mouseClick(CancelButton, Qt::LeftButton);
        QTRY_COMPARE(Scheduler.Snapshot(Cancelled.Value().jobId_).Value().state_, JobState::Cancelled);
        QVERIFY(QFile::exists(Destination + "/second.txt"));
        QTest::qWait(150); QTRY_VERIFY(!Model->IsLoading());
        Panel->ChangeCurrentPath(QDir::toNativeSeparators(Source), {});
        QTRY_VERIFY(!Model->IsLoading());
        auto Conflict = Bridge->Dispatch("hc.file.copy", {{"destination", Destination}}, "test", IndexFor("second.txt"));
        QTRY_VERIFY(Conflict.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        const auto Conflicting = Conflict.get(); QVERIFY(Conflicting.HasValue());
        const auto ConflictId = Conflicting.Value().jobId_;
        QTRY_COMPARE(Scheduler.Snapshot(ConflictId).Value().state_, JobState::WaitingForUser);
        auto* PauseButton = main_->findChild<QPushButton*>("hc.jobs.pause");
        QVERIFY(PauseButton); QTRY_VERIFY(PauseButton->isEnabled());
        QTest::mouseClick(PauseButton, Qt::LeftButton);
        QTRY_VERIFY(Scheduler.Snapshot(ConflictId).Value().pauseRequested_);
        QTest::mouseClick(PauseButton, Qt::LeftButton);
        QTRY_VERIFY(!Scheduler.Snapshot(ConflictId).Value().pauseRequested_);
        auto* NewName = main_->findChild<QLineEdit*>("hc.jobs.newName");
        auto* RenameChoice = main_->findChild<QPushButton*>("hc.jobs.choice." + QString::number(static_cast<int>(InteractionChoice::Rename)));
        QVERIFY(NewName); QVERIFY(RenameChoice); QTRY_VERIFY(NewName->isVisible());
        NewName->setText("alternate.txt"); QTest::mouseClick(RenameChoice, Qt::LeftButton);
        QTRY_COMPARE(Scheduler.Snapshot(ConflictId).Value().state_, JobState::Completed);
        QVERIFY(QFile::exists(Destination + "/alternate.txt"));
        QVERIFY(QFile::exists(Destination + "/second.txt"));
        QTest::qWait(150); QTRY_VERIFY(!Model->IsLoading());
        auto Mkdir = Bridge->Dispatch("hc.file.mkdir", {{"destination", Destination + "/folder"}}, "test", IndexFor("second.txt"));
        QTRY_VERIFY(Mkdir.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        const auto Made = Mkdir.get(); QVERIFY(Made.HasValue());
        QTRY_COMPARE(Scheduler.Snapshot(Made.Value().jobId_).Value().state_, JobState::Completed);
        QTest::qWait(150); QTRY_VERIFY(!Model->IsLoading());
        auto Move = Bridge->Dispatch("hc.file.move", {{"destination", Destination + "/folder"}}, "test", IndexFor("second.txt"));
        QTRY_VERIFY(Move.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        const auto Moved = Move.get(); QVERIFY(Moved.HasValue());
        QTRY_COMPARE(Scheduler.Snapshot(Moved.Value().jobId_).Value().state_, JobState::Completed);
        QVERIFY(!QFile::exists(Source + "/second.txt"));
        QVERIFY(QFile::exists(Destination + "/folder/second.txt"));
        QTest::qWait(150); QTRY_VERIFY(!Model->IsLoading());
        auto* Jobs = main_->findChild<QDialog*>("hc.fileJobs");
        QVERIFY(Jobs); QVERIFY(Jobs->grab().save(QCoreApplication::applicationDirPath() + "/m4-jobs.png"));
        Jobs->hide();
        Panel->ChangeCurrentPath(unc_, {});
        QTRY_VERIFY(!Model->IsLoading());
    }

    void materializationCopiesRemoteContent()
    {
        using namespace hc::storage;
        QTemporaryDir Remote(unc_ + "/hc-materializer-test-XXXXXX"); QVERIFY(Remote.isValid());
        QTemporaryDir Local; QVERIFY(Local.isValid());
        QFile File(Remote.filePath("원격.txt")); QVERIFY(File.open(QIODevice::WriteOnly));
        const auto Bytes = QByteArray(65536, 'r'); QCOMPARE(File.write(Bytes), Bytes.size()); File.close();
        auto Reader = MakeNativeReadProvider(LocatorKind::Smb);
        auto Entry = Reader->Resolve({LocatorKind::Smb, QDir::toNativeSeparators(File.fileName()).toStdString()}, {}).get(); QVERIFY(Entry.HasValue());
        auto Service = MakeNativeMaterializer(Local.path().toStdString()); QVERIFY(Service.HasValue());
        auto Made = Service.Value()->Materialize({std::make_shared<ProviderContentSource>(Reader, Entry.Value())}, {});
        bool Tick = false; QTimer::singleShot(0, this, [&Tick] { Tick = true; });
        QTRY_VERIFY(Tick);
        QTRY_VERIFY(Made.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        auto Lease = Made.get(); QVERIFY(Lease.HasValue());
        QFile Copy(QString::fromStdString(Lease.Value()->LocalPath())); QVERIFY(Copy.open(QIODevice::ReadOnly)); QCOMPARE(Copy.readAll(), Bytes); Copy.close();
        Lease.Value().reset(); QVERIFY(Service.Value()->DrainCleanup().errors_.empty());
        QVERIFY(!QFile::exists(Copy.fileName())); QVERIFY(QFile::exists(File.fileName()));
    }


    void r10PermissionChoicesUi() {
        using namespace hc::ui;
        PermissionRequest R;R.screen_="A03";R.source_=QStringLiteral("//server/share/한글 원본 파일.txt");R.destination_=QStringLiteral("C:/Program Files/HyperCommander fixture/한글 대상.txt");R.reason_=QStringLiteral("원본은 본체 계정으로 읽고 로컬 대상 저장만 승격합니다. 이 화면은 응답 경로 검증용입니다.");
        R.canElevate_=R.canApplyAll_=R.canSkip_=R.canCopyOnly_=R.canRetry_=true;R.jobId_="fixture-streamed-copy-job";
        PermissionDialog D(R,main_);D.show();QVERIFY(QTest::qWaitForWindowExposed(&D));auto* Same=D.findChild<QCheckBox*>("hc.permission.same");QVERIFY(Same&&Same->isEnabled());Same->setChecked(true);
        const auto Out=qEnvironmentVariable("HC_UI_ARTIFACT_DIR");if(!Out.isEmpty())QVERIFY(D.grab().save(QDir(Out).filePath("r10-provider-to-local-"+QString::number(D.devicePixelRatioF())+".png")));
        D.AcceptButton()->click();QCOMPARE(D.ResultDecision(),PermissionDialog::Elevate);QVERIFY(D.ApplyAll());
        PermissionDialog Skip(R,main_);Skip.findChild<QCheckBox*>("hc.permission.same")->setChecked(true);Skip.findChild<QPushButton*>("hc.permission.skip")->click();QCOMPARE(Skip.ResultDecision(),PermissionDialog::Skip);QVERIFY(Skip.ApplyAll());
        PermissionDialog Copy(R,main_);Copy.findChild<QPushButton*>("hc.permission.copyOnly")->click();QCOMPARE(Copy.ResultDecision(),PermissionDialog::CopyOnly);
        PermissionDialog Retry(R,main_);Retry.findChild<QPushButton*>("hc.permission.retry")->click();QCOMPARE(Retry.ResultDecision(),PermissionDialog::Retry);
        auto* Acl=PermissionSettingsPage(main_);QVERIFY(Acl->findChild<QPushButton*>("hc.permissions.restore"));delete Acl;
    }

    void permissionsUi()
    {
        using namespace hc::ui;
        QTemporaryDir Temp; QVERIFY(Temp.isValid());
        QVERIFY(hc::filetools::Create(Temp.path(),"source.txt",1,false).isEmpty());
        auto& Theme=fm::style::ThemeManager::instance(); const auto Original=Theme.scheme();
        const auto Restore=qScopeGuard([&]{Theme.setScheme(Original);});
        const auto Save=[&](QWidget* W,const QString& Name) {
            const auto Out=qEnvironmentVariable("HC_UI_ARTIFACT_DIR"); if(Out.isEmpty())return;
            for(bool Dark:{false,true}) { Theme.setScheme(Dark?fm::style::ThemeManager::Scheme::Dark:fm::style::ThemeManager::Scheme::Light); QTest::qWait(200);
                QVERIFY(W->grab().save(QDir(Out).filePath(Name+(Dark?"-dark-":"-light-")+QString::number(W->devicePixelRatioF())+".png"))); }
        };
        auto* Proxy=qobject_cast<QProxyStyle*>(qApp->style()); QVERIFY(Proxy);
        fm::ui::Button Probe("primary"); Probe.setRole(fm::ui::Button::Primary); Probe.resize(180,36);
        QStyleOptionButton PaintOption; PaintOption.initFrom(&Probe); PaintOption.rect=Probe.rect(); PaintOption.state=QStyle::State_Enabled;
        QImage Product(Probe.size(),QImage::Format_ARGB32_Premultiplied), Reference(Product.size(),Product.format()); Product.fill(Qt::transparent);Reference.fill(Qt::transparent);
        { QPainter P(&Product); Proxy->drawControl(QStyle::CE_PushButtonBevel,&PaintOption,&P,&Probe); }
        { QPainter P(&Reference); Proxy->baseStyle()->drawControl(QStyle::CE_PushButtonBevel,&PaintOption,&P,&Probe); }
        QCOMPARE(Product,Reference); // Product proxy must preserve Watercolor rendering.
        QVERIFY(main_->findChild<QAction*>("hc.menu.permissions"));
        ShowPermissionInspection({Temp.path(),Temp.filePath("source.txt")},main_);
        auto* Inspection=static_cast<DialogShell*>(main_->findChild<QDialog*>("hc.permission.A01")); QVERIFY(Inspection);
        auto* Table=Inspection->findChild<QTableWidget*>(); QTRY_COMPARE(Table->rowCount(),2); Save(Inspection,"r10-A01-real-inspection"); Inspection->close();
        for(int I=2;I<=8;++I) {
            PermissionRequest R; R.screen_=QString("A%1").arg(I,2,10,QChar('0')); R.source_=Temp.filePath("source.txt"); R.destination_=Temp.filePath("target.txt");
            R.reason_=QStringLiteral("검증용 권한 상태 · 실제 UAC 승인 결과 아님"); R.canSkip_=I<=4;
            R.canElevate_=I<=6;R.canApplyAll_=I<=4;R.jobId_="fixture-current-job";R.canCopyOnly_=I==3;R.canRetry_=I<=6;
            if(I==5)R.elevatedRequest_=hc::permissions::Request("rename",Temp.path(),"target.txt",R.source_);
            if(I==6)R.elevatedRequest_=hc::permissions::Request("create",Temp.path(),"target.txt",{},1,false);
            PermissionDialog D(R,main_); D.show(); QVERIFY(QTest::qWaitForWindowExposed(&D));
            if(I==7)QTRY_VERIFY(!D.findChildren<QLabel*>().isEmpty());
            Save(&D,"r10-"+R.screen_+"-fixture");
            QCOMPARE(D.AcceptButton()->isEnabled(),I<=6);
            for(auto* B:D.findChildren<QPushButton*>()) if(B->isVisible()) QVERIFY(D.rect().contains(QRect(B->mapTo(&D,QPoint()),B->size())));
            if(I<=4) { D.findChild<QPushButton*>("hc.permission.skip")->click(); QCOMPARE(D.ResultDecision(),PermissionDialog::Skip); }
            else QTest::keyClick(&D,Qt::Key_Escape);
        }
        QMainOpts Options(main_); Options.show(); Options.LoadSettings();
        auto* Page=Options.findChild<QWidget*>("hc.settings.permissions"); QVERIFY(Page);
        auto* Navigation=Options.findChild<QListWidget*>("lstOptCate"); QVERIFY(Navigation);
        const auto Items=Navigation->findItems(QStringLiteral("관리자 권한"),Qt::MatchExactly); QCOMPARE(Items.size(),1); Navigation->setCurrentItem(Items.first());
        Save(&Options,"r10-S08-settings");
        auto* Allow=Page->findChild<QCheckBox*>("hc.permissions.allow"); QVERIFY(Allow);
        const bool Before=Allow->isChecked(); Allow->setChecked(!Before); QVERIFY(Options.SaveSettings());
        QCOMPARE(hc::settings::AppSettings().Read(hc::settings::Section::Options).toObject()["allowElevation"].toBool(),!Before);
        Allow->setChecked(Before); QVERIFY(Options.SaveSettings()); QTest::keyClick(&Options,Qt::Key_Escape);
        QVERIFY(!QFileInfo::exists(Temp.filePath("target.txt"))); // Screens never request UAC in automation.
    }

    void fileToolsUi()
    {
        using namespace hc::ui;
        const auto Workspace = main_->CaptureWorkspace();
        QTemporaryDir Temp; QVERIFY(Temp.isValid());
        const auto RestoreWorkspace = qScopeGuard([&] { main_->RestoreWorkspace(Workspace); });
        for (auto* Panel : panels_) { Panel->ChangeCurrentPath(QDir::toNativeSeparators(Temp.path()),{}); QTRY_VERIFY(!model(Panel)->IsLoading()); }
        emit panels_.front()->sig_NotifyPanelActivated();
        auto& Theme = fm::style::ThemeManager::instance(); const auto OriginalScheme = Theme.scheme();
        const auto Restore = qScopeGuard([&] { Theme.setScheme(OriginalScheme); });
        const auto Save = [&](QWidget* W, const QString& Name) {
            const auto Out = qEnvironmentVariable("HC_UI_ARTIFACT_DIR"); if (Out.isEmpty()) return;
            for (const bool Dark : {false,true}) { Theme.setScheme(Dark ? fm::style::ThemeManager::Scheme::Dark : fm::style::ThemeManager::Scheme::Light); QCoreApplication::processEvents();
                QVERIFY(W->grab().save(QDir(Out).filePath(Name + (Dark ? "-dark-" : "-light-") + QString::number(W->devicePixelRatioF()) + ".png"))); }
        };
        QVERIFY(main_->findChild<QAction*>("hc.menu.file.new")); QVERIFY(main_->findChild<QAction*>("hc.menu.file.batchRename"));
        for (const auto& Pair : {qMakePair("hc.menu.file.new", "hc.newFile"), qMakePair("hc.menu.file.batchRename", "hc.batchRename")}) {
            QTRY_VERIFY(main_->findChild<QAction*>(Pair.first)->isEnabled());
            bool Opened = false;
            QTimer::singleShot(0, main_, [&] { for (auto* W : QApplication::topLevelWidgets()) if (W->objectName() == Pair.second) {
                Opened = true; qobject_cast<QDialog*>(W)->reject(); break;
            } });
            main_->findChild<QAction*>(Pair.first)->trigger(); QVERIFY(Opened);
        }
        NewFileDialog Create(Temp.path(), true, main_); Create.show(); QTest::qWait(30);
        auto* Name = Create.findChild<QLineEdit*>("hc.newFile.name"); QVERIFY(Name); Name->setText("document");
        for (int I=0; I<8; ++I) { auto* Type = Create.findChild<QComboBox*>("hc.newFile.type"); QVERIFY(Type); Type->setCurrentIndex(I); Save(&Create, "p5-template-" + QString::number(I)); }
        Create.findChild<QComboBox*>("hc.newFile.type")->setCurrentIndex(6); Name->setText("created.json");
        Create.findChild<QCheckBox*>("hc.newFile.bom")->setChecked(true); Create.AcceptButton()->click(); QTRY_COMPARE(Create.result(), int(QDialog::Accepted));
        QFile Json(Temp.filePath("created.json")); QVERIFY(Json.open(QIODevice::ReadOnly)); QVERIFY(Json.readAll().startsWith(QByteArray::fromHex("efbbbf"))); Json.close();
        NewFileDialog Conflict(Temp.path(), true, main_); Conflict.show(); Conflict.findChild<QLineEdit*>("hc.newFile.name")->setText("created.json"); Conflict.AcceptButton()->click();
        QTRY_VERIFY(Conflict.AcceptButton()->isEnabled()); QVERIFY(Conflict.CreatedPath().isEmpty()); Save(&Conflict, "p5-create-conflict"); QTest::keyClick(&Conflict, Qt::Key_Escape);
        QStringList Paths;
        for (int I=0; I<12; ++I) { const auto Path=Temp.filePath(I == 11 ? QString("file_11.txt") : QString("sample_%1.txt").arg(I,2,10,QChar('0'))); QFile F(Path); QVERIFY(F.open(QIODevice::WriteOnly)); F.write(QByteArray::number(I)); Paths.append(Path); }
        { QFile Collision(Temp.filePath("file_10.txt")); QVERIFY(Collision.open(QIODevice::WriteOnly)); Collision.write("external"); }
        BatchRenameDialog Rename(Paths, true, main_); Rename.show();
        auto* Mask=Rename.findChild<QLineEdit*>("hc.rename.mask"); auto* Preview=static_cast<fm::ui::RenameComparisonView*>(Rename.findChild<QTableWidget*>("hc.rename.preview"));
        QVERIFY(Mask && Preview); QTRY_VERIFY(Mask->isEnabled() && Preview->rowCount()==12);
        Save(&Rename, "p5-rename-unchanged");
        Rename.findChild<QLineEdit*>("hc.rename.find")->setText("sample_"); Rename.findChild<QLineEdit*>("hc.rename.replace")->setText("file_");
        QVERIFY(!Rename.AcceptButton()->isEnabled()); Save(&Rename, "p5-rename-mixed");
        QTRY_COMPARE(Rename.findChild<QLabel*>("hc.rename.summary")->text(), QStringLiteral("12개 중 변경 10 · 충돌/오류 1 · 변경 없음 1"));
        Rename.findChild<QLineEdit*>("hc.rename.find")->clear(); Rename.findChild<QLineEdit*>("hc.rename.replace")->clear();
        Mask->setText("file_[C]"); QTRY_VERIFY(Rename.AcceptButton()->isEnabled());
        auto* Preset = Rename.findChild<QComboBox*>("hc.rename.preset"); Preset->setEditText("p5-fixture");
        Rename.findChild<QPushButton*>("hc.rename.presetSave")->click();
        hc::settings::Repository Reopened(QFileInfo(hc::settings::AppSettings().FileName()).absolutePath());
        QVERIFY(Reopened.Read(hc::settings::Section::Options).toObject()["renamePresets"].toObject().contains("p5-fixture"));
        Mask->setText("temporary"); Rename.findChild<QPushButton*>("hc.rename.presetLoad")->click(); QCOMPARE(Mask->text(), QString("file_[C]"));
        Rename.findChild<QPushButton*>("hc.rename.presetDelete")->click();
        QVERIFY(!hc::settings::AppSettings().Read(hc::settings::Section::Options).toObject()["renamePresets"].toObject().contains("p5-fixture"));
        auto* Modes=Rename.findChild<fm::ui::SegmentedControl*>("hc.rename.mode"); Modes->setCurrentIndex(0); Save(&Rename, "p5-rename-one"); Modes->setCurrentIndex(1); Save(&Rename, "p5-rename-two");
        Mask->setText("same"); QVERIFY(!Rename.AcceptButton()->isEnabled()); Save(&Rename, "p5-rename-conflict");
        Mask->setText("file_[C]"); QTRY_VERIFY(Rename.AcceptButton()->isEnabled()); Rename.AcceptButton()->click(); auto* Undo=Rename.findChild<QPushButton*>("hc.rename.undo"); auto* Redo=Rename.findChild<QPushButton*>("hc.rename.redo");
        QTRY_VERIFY(Undo->isEnabled()); QVERIFY(QFileInfo::exists(Temp.filePath("file_001.txt"))); QVERIFY(!QFileInfo::exists(Paths[0])); Save(&Rename, "p5-rename-done");
        Undo->click(); QTRY_VERIFY(Redo->isEnabled()); QVERIFY(QFileInfo::exists(Paths[0])); Save(&Rename, "p5-rename-undo");
        Redo->click(); QTRY_VERIFY(Undo->isEnabled()); QVERIFY(QFileInfo::exists(Temp.filePath("file_012.txt"))); QTest::keyClick(&Rename, Qt::Key_Escape);
    }
    void fileColorFinalReviewUi()
    {
        using namespace hc::settings;
        auto& Settings = AppSettings(); const auto Original = Settings.Object(); auto* Manager = TyStColorSchemeMgr::GetInstance();
        auto& Theme = fm::style::ThemeManager::instance();
        const bool NativeDialogs = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs); QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
        const auto Restore = qScopeGuard([&] { Settings.DiscardEdit(); Settings.Replace(Original); Settings.Commit(); Manager->Refresh(); emit Manager->sigColorSchemeChanged(Manager->GetColorScheme(Manager->GetCurrentColorScheme())); QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, NativeDialogs); });
        QMainOpts Dialog(main_); Dialog.show(); QTest::qWait(50); Dialog.findChild<QListWidget*>("lstOptCate")->setCurrentRow(4);
        auto* Page = static_cast<hc::ui::FileColorRulesPage*>(Dialog.findChild<QWidget*>("hc.settings.fileColors"));
        Page->Load(FileColorRules::Empty()); Page->findChild<QPushButton*>("hc.rules.addExamples")->click(); Page->findChild<QRadioButton*>("hc.rules.merge")->click();
        auto* Groups = Page->findChild<QListWidget*>("hc.rules.groups"); Groups->setCurrentRow(5); const auto Expected = Page->Document();
        const auto Source = Groups->currentItem()->data(Qt::UserRole); Groups->setFocus(); QTest::keyClick(Groups, Qt::Key_Up, Qt::AltModifier);
        QCOMPARE(Groups->currentRow(), 4); QCOMPARE(Groups->currentItem()->data(Qt::UserRole), Source); QTest::keyClick(Groups, Qt::Key_Down, Qt::AltModifier); QCOMPARE(Page->Document(), Expected);
        Page->findChild<QPushButton*>("hc.rules.copy")->click(); QCOMPARE(Groups->count(), 11); QVERIFY(Groups->currentItem()->data(Qt::UserRole) != Source); Page->findChild<QPushButton*>("hc.rules.delete")->click(); QCOMPARE(Page->Document(), Expected);
        Dialog.raise(); Dialog.activateWindow(); Groups->setFocus(); QTRY_VERIFY(Dialog.isActiveWindow());
        QTest::keyClick(Groups, Qt::Key_W, Qt::AltModifier); QTRY_COMPARE(Groups->count(), 11); Page->findChild<QPushButton*>("hc.rules.delete")->click(); QCOMPARE(Page->Document(), Expected); Groups->setCurrentRow(5);
        for (int I = 0; I < 4; ++I) { auto* Control = Page->findChild<fm::ui::ColorPickButton*>("hc.rules.color." + QString::number(I)); QVERIFY(!Control->accessibleName().isEmpty()); }
        for (const auto* Name : {"bold", "italic", "underline", "strike"}) { auto* Control = Page->findChild<QPushButton*>(QString("hc.rules.font.") + Name); QVERIFY(Control->focusPolicy() != Qt::NoFocus); QVERIFY(!Control->accessibleName().isEmpty()); }
        auto* Name = Page->findChild<QLineEdit*>("hc.rules.name"); Name->setFocus();
        for (int I = 0; I < 36; ++I) { auto* Focus = QApplication::focusWidget(); QVERIFY(Focus && Dialog.isAncestorOf(Focus)); QTest::keyClick(Focus, Qt::Key_Tab); }
        QCOMPARE(Page->Document(), Expected);
        QTemporaryDir Files; QVERIFY(Files.isValid()); const auto Path = Files.filePath("groups.json");
        const auto Pick = [&](const char* ButtonName) {
            bool Chosen = false, TimedOut = false; QTimer Driver; Driver.setInterval(20); QElapsedTimer Elapsed; Elapsed.start();
            connect(&Driver, &QTimer::timeout, &Dialog, [&] { for (auto* Picker : Dialog.findChildren<QFileDialog*>()) if (Picker->isVisible()) {
                if (Elapsed.elapsed() > 5000) { TimedOut = true; Picker->reject(); continue; }
                if (!Chosen) { Chosen = true; Picker->setDirectory(Files.path()); }
                if (auto* FileName = Picker->findChild<QLineEdit*>("fileNameEdit")) FileName->setText(Path);
                QMetaObject::invokeMethod(Picker, "accept", Qt::QueuedConnection);
            } }); Driver.start(); Page->findChild<QPushButton*>(ButtonName)->click(); Driver.stop(); QVERIFY(Chosen); QVERIFY(!TimedOut);
        };
        Pick("hc.rules.export"); QFile Export(Path); QVERIFY(Export.open(QIODevice::ReadOnly)); const auto Bytes = Export.readAll(); Export.close();
        QCOMPARE(QJsonDocument::fromJson(Bytes).object(), Expected); QVERIFY(Bytes.contains("\r\n")); QVERIFY(!QByteArray(Bytes).replace("\r\n", "").contains('\n'));
        Page->Load(FileColorRules::Empty()); Pick("hc.rules.import"); QCOMPARE(Page->Document(), Expected);
        QFile Invalid(Path); QVERIFY(Invalid.open(QIODevice::WriteOnly | QIODevice::Truncate)); Invalid.write("{\"version\":99,\"groups\":[]}"); Invalid.close(); Pick("hc.rules.import"); QCOMPARE(Page->Document(), Expected);
        Groups->setCurrentRow(5); auto* Table = Page->findChild<QTableWidget*>("hc.rules.previewTable"); Table->selectRow(4);
        const auto Capture = [&](const QString& Kind) { const auto Out = qEnvironmentVariable("HC_UI_ARTIFACT_DIR"); if (Out.isEmpty()) return; QDir().mkpath(Out); QVERIFY(Dialog.grab().save(QDir(Out).filePath(Kind + '-' + QString::number(Dialog.devicePixelRatioF()) + ".png"))); };
        for (const auto Design : {Theme.design()}) {
            // Exercise the installed product style. Replacing QApplication style directly would destroy its Qtitan proxy.
            for (const bool Dark : {false, true}) {
                emit Manager->sigColorSchemeChanged(Manager->GetColorScheme(Dark ? QStringLiteral("기본 다크") : QStringLiteral("기본 라이트"))); Page->findChild<QCheckBox*>("hc.rules.previewDark")->setChecked(Dark); QTest::qWait(30);
                const auto Prefix = QString("review-%1-%2").arg(Design == fm::style::Design::Watercolor ? "watercolor" : "standard", Dark ? "dark" : "light");
                for (auto* Scroll : Page->findChildren<QScrollArea*>()) Scroll->verticalScrollBar()->setValue(0); QCoreApplication::processEvents(); Capture(Prefix + "-top");
                for (auto* Scroll : Page->findChildren<QScrollArea*>()) Scroll->verticalScrollBar()->setValue(Scroll->verticalScrollBar()->maximum()); QCoreApplication::processEvents(); Capture(Prefix + "-preview");
                QCOMPARE(Table->verticalScrollBar()->maximum(), 0);
                auto* Sidebar = Dialog.findChild<QWidget*>("hc.settings.sidebar"); auto* Search = Dialog.findChild<QLineEdit*>("hc.settings.search");
                auto* GroupCard = Page->findChild<QWidget*>("hc.rules.groupsCard"); auto* Stack = Dialog.findChild<QStackedWidget*>("stkOpts");
                QVERIFY(Sidebar && Search && GroupCard && Stack);
                const int NavigationWidth = Sidebar->width(), GroupWidth = GroupCard->width(); const auto ContentX = Stack->mapTo(&Dialog, QPoint()).x();
                const auto Full = Dialog.size(); Dialog.resize(Full.width() + 300, Full.height()); QCoreApplication::processEvents();
                QCOMPARE(Sidebar->width(), NavigationWidth); QCOMPARE(Search->width(), Groups->window()->findChild<QListWidget*>("lstOptCate")->width());
                QCOMPARE(GroupCard->width(), GroupWidth); QCOMPARE(Stack->mapTo(&Dialog, QPoint()).x(), ContentX); Capture(Prefix + "-wide");
                Dialog.findChild<QListWidget*>("lstOptCate")->setCurrentRow(0); QCoreApplication::processEvents(); QCOMPARE(Stack->mapTo(&Dialog, QPoint()).x(), ContentX); Capture(Prefix + "-general-wide");
                Dialog.findChild<QListWidget*>("lstOptCate")->setCurrentRow(4);
                Dialog.resize(900, 650); QCoreApplication::processEvents(); const auto Small = Dialog.size();
                for (int I = 0; I < 6; ++I) { Dialog.move(Dialog.pos() + QPoint(I % 2 ? -8 : 8, 0)); QCoreApplication::processEvents(); QCOMPARE(Dialog.size(), Small); }
                Dialog.resize(Full); QCoreApplication::processEvents(); QCOMPARE(Dialog.size(), Full);
            }
        }
        Page->Load(Expected); QVERIFY(Dialog.SaveSettings()); Name->setText("취소할 이름"); Dialog.reject();
        Repository Restart(QFileInfo(Settings.FileName()).absolutePath()); QCOMPARE(Restart.Read(Section::Options).toObject()["fileColorRules"].toObject(), Expected);
    }

    void fileColorPreviewUi()
    {
        using namespace hc::settings;
        auto& Settings = AppSettings(); const auto Original = Settings.Object(); const auto Workspace = main_->CaptureWorkspace(); auto* Manager = TyStColorSchemeMgr::GetInstance();
        const auto Restore = qScopeGuard([&] { Settings.DiscardEdit(); Settings.Replace(Original); Settings.Commit(); Manager->Refresh(); emit Manager->sigColorSchemeChanged(Manager->GetColorScheme(Manager->GetCurrentColorScheme())); main_->RestoreWorkspace(Workspace); });
        QTemporaryDir Directory; for (const auto* Name : {"BandedPanelView.cpp", "placeholder-notes.txt", "Qt Creator 18.lnk"}) { QFile File(Directory.filePath(Name)); QVERIFY(File.open(QIODevice::WriteOnly)); if (qstrcmp(Name, "placeholder-notes.txt")) File.write("data"); QVERIFY(File.setFileTime(QDateTime::currentDateTimeUtc().addSecs(qstrcmp(Name, "BandedPanelView.cpp") ? -172800 : -3600), QFileDevice::FileModificationTime)); }
        panels_.front()->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {}); QTRY_VERIFY(!model(panels_.front())->IsLoading() && names(model(panels_.front())).contains("BandedPanelView.cpp"));
        QMainOpts Dialog(main_); Dialog.show(); QTest::qWait(50); Dialog.findChild<QListWidget*>("lstOptCate")->setCurrentRow(4);
        auto* Page = static_cast<hc::ui::FileColorRulesPage*>(Dialog.findChild<QWidget*>("hc.settings.fileColors")); auto Doc = FileColorRules::Empty();
        QJsonObject OriginalGroup{{"id", "user-group"}, {"name", "문서"}, {"join", "any"}, {"future", 42}, {"conditions", QJsonArray{QJsonObject{{"type", "extension"}, {"value", "foo"}}}}};
        Doc["groups"] = QJsonArray{OriginalGroup}; Page->Load(Doc); auto* Add = Page->findChild<QPushButton*>("hc.rules.addExamples"); QVERIFY(Add); Add->click();
        QCOMPARE(Page->Document()["groups"].toArray().size(), 11); QCOMPARE(Page->Document()["groups"].toArray()[0].toObject()["future"], QJsonValue(42)); QVERIFY(Page->Validate());
        const auto Once = Page->Document(); Add->click(); QCOMPARE(Page->Document(), Once);
        auto* Table = Page->findChild<QTableWidget*>("hc.rules.previewTable"); QVERIFY(Table); QCOMPARE(Table->rowCount(), 6);
        QCOMPARE(Table->item(0, 2)->text(), QStringLiteral("소스 코드")); QVERIFY(!Table->item(2, 0)->font().strikeOut());
        Page->findChild<QRadioButton*>("hc.rules.merge")->click();
        QCOMPARE(Table->item(0, 2)->text(), QStringLiteral("소스 코드 + 최근 24시간에 바뀜")); QVERIFY(Table->item(0, 0)->font().bold());
        QVERIFY(Table->item(2, 2)->text().contains(QStringLiteral("문서 2 + 빈 파일"))); QVERIFY(Table->item(2, 0)->font().strikeOut()); QVERIFY(Table->item(3, 0)->font().underline());
        QVERIFY(Table->item(0, 2)->toolTip().contains(QStringLiteral("배경색: 최근 24시간에 바뀜")));
        QVERIFY(Dialog.SaveSettings()); const auto Saved = Page->Document();
        auto* Model = static_cast<CFSModel*>(model(panels_.front()));
        for (int Row = 0; Row < Model->rowCount(); ++Row) { const auto Index = Model->index(Row, 0); const auto Name = Model->GetName(Index);
            if (Name == "BandedPanelView.cpp") { QTRY_VERIFY(Index.data(Qt::FontRole).value<QFont>().bold()); QVERIFY(Index.data(Qt::BackgroundRole).value<QColor>().isValid()); }
            if (Name == "placeholder-notes.txt") QTRY_VERIFY(Index.data(Qt::FontRole).value<QFont>().strikeOut());
            if (Name == "Qt Creator 18.lnk") QTRY_VERIFY(Index.data(Qt::FontRole).value<QFont>().underline());
        }
        auto* Groups = Page->findChild<QListWidget*>("hc.rules.groups"); Groups->setCurrentRow(6);
        auto* Dark = Page->findChild<QCheckBox*>("hc.rules.previewDark");
        for (const bool IsDark : {false, true}) {
            emit Manager->sigColorSchemeChanged(Manager->GetColorScheme(IsDark ? QStringLiteral("기본 다크") : QStringLiteral("기본 라이트"))); Dark->setChecked(IsDark);
            Table->selectRow(4); QCoreApplication::processEvents();
            for (auto* Scroll : Page->findChildren<QScrollArea*>()) if (Scroll->isAncestorOf(Table)) Scroll->ensureWidgetVisible(Table);
            QCoreApplication::processEvents(); const auto Out = qEnvironmentVariable("HC_UI_ARTIFACT_DIR"); if (!Out.isEmpty()) { QDir().mkpath(Out); QVERIFY(Dialog.grab().save(QDir(Out).filePath(QString("preview-%1-%2.png").arg(IsDark ? "dark" : "light").arg(Dialog.devicePixelRatioF())))); }
        }
        Page->findChild<QToolButton*>("hc.rules.customToggle")->setChecked(true); Page->findChild<QLineEdit*>("hc.rules.sample")->setText("custom.cpp");
        QCOMPARE(Page->Document(), Saved); Dialog.reject(); Repository Restart(QFileInfo(Settings.FileName()).absolutePath()); QCOMPARE(Restart.Read(Section::Options).toObject()["fileColorRules"].toObject(), Saved);
    }

    void fileColorAppearanceUi()
    {
        using namespace hc::settings;
        auto& Settings = AppSettings(); const auto Original = Settings.Object(); auto* Manager = TyStColorSchemeMgr::GetInstance();
        const auto Restore = qScopeGuard([&] { Settings.DiscardEdit(); Settings.Replace(Original); Settings.Commit(); Manager->Refresh(); emit Manager->sigColorSchemeChanged(Manager->GetColorScheme(Manager->GetCurrentColorScheme())); });
        QMainOpts Dialog(main_); Dialog.show(); QTest::qWait(50); Dialog.findChild<QListWidget*>("lstOptCate")->setCurrentRow(4);
        auto* Page = static_cast<hc::ui::FileColorRulesPage*>(Dialog.findChild<QWidget*>("hc.settings.fileColors"));
        auto Doc = FileColorRules::Empty(); QJsonObject G{{"id", "appearance"}, {"name", "색상 보존"}, {"join", "any"}, {"conditions", QJsonArray{QJsonObject{{"type", "extension"}, {"value", "txt"}}}},
            {"light", QJsonObject{{"foreground", "#80123456"}, {"future", "light"}}}, {"dark", QJsonObject{{"foreground", "#abcdef"}, {"background", "#223344"}, {"future", "dark"}}}, {"font", QJsonObject{{"bold", false}, {"future", "font"}}}};
        Doc["groups"] = QJsonArray{G}; Page->Load(Doc); QCOMPARE(Page->Document(), Doc);
        auto* Auto = Page->findChild<QCheckBox*>("hc.rules.autoDark"); auto* Dark = Page->findChild<fm::ui::ColorPickButton*>("hc.rules.color.1");
        QVERIFY(Auto && Dark); Auto->setChecked(true); QVERIFY(!Dark->isEnabled());
        QCOMPARE(Page->Document()["groups"].toArray()[0].toObject()["dark"], G["dark"]);
        QCOMPARE(Page->Document()["groups"].toArray()[0].toObject()["light"], G["light"]);
        QVERIFY(Dark->color()); QCOMPARE(Dark->color()->alpha(), 128);
        Auto->setChecked(false); QVERIFY(Dark->isEnabled()); QCOMPARE(*Dark->color(), QColor("#abcdef"));
        auto* Bold = Page->findChild<QPushButton*>("hc.rules.font.bold"); QVERIFY(Bold); Bold->setFocus(); QTest::keyClick(Bold, Qt::Key_Space);
        QVERIFY(!Page->Document()["groups"].toArray()[0].toObject()["font"].toObject().contains("bold"));
        QTest::keyClick(Bold, Qt::Key_Space); QCOMPARE(Page->Document()["groups"].toArray()[0].toObject()["font"].toObject()["bold"], QJsonValue(true));
        QTest::keyClick(Bold, Qt::Key_Space); QCOMPARE(Page->Document()["groups"].toArray()[0].toObject()["font"].toObject()["bold"], QJsonValue(false));
        QCOMPARE(Page->Document()["groups"].toArray()[0].toObject()["font"].toObject()["future"], QJsonValue("font"));
        Auto->setChecked(true); QVERIFY(Dialog.SaveSettings()); const auto Saved = Page->Document();
        Repository Restart(QFileInfo(Settings.FileName()).absolutePath()); QCOMPARE(Restart.Read(Section::Options).toObject()["fileColorRules"].toObject(), Saved);
        Page->Load(Saved); Auto->setChecked(false); QCOMPARE(*Dark->color(), QColor("#abcdef"));
        for (const bool IsDark : {false, true}) {
            emit Manager->sigColorSchemeChanged(Manager->GetColorScheme(IsDark ? QStringLiteral("기본 다크") : QStringLiteral("기본 라이트"))); QCoreApplication::processEvents();
            for (int I = 0; I < 2; ++I) QVERIFY(Page->findChild<QLabel*>("hc.rules.contrast." + QString::number(I))->text().contains(":1"));
            const auto Out = qEnvironmentVariable("HC_UI_ARTIFACT_DIR"); if (!Out.isEmpty()) { QDir().mkpath(Out); QVERIFY(Dialog.grab().save(QDir(Out).filePath(QString("appearance-%1-%2.png").arg(IsDark ? "dark" : "light").arg(Dialog.devicePixelRatioF())))); }
        }
        Dialog.reject(); QCOMPARE(Settings.Read(Section::Options).toObject()["fileColorRules"].toObject(), Saved);
    }

    void todoBugFileColorNormalization()
    {
        using namespace hc::settings;
        auto& Settings = AppSettings(); const auto Before = Settings.Object(); const auto Workspace = main_->CaptureWorkspace();
        auto* Colors = TyStColorSchemeMgr::GetInstance();
        const auto Restore = qScopeGuard([&] { Settings.DiscardEdit(); Settings.Replace(Before); Settings.Commit(); Colors->Refresh(); emit Colors->sigColorSchemeChanged(Colors->GetColorScheme(Colors->GetCurrentColorScheme())); main_->RestoreWorkspace(Workspace); });
        const auto Nfc = QStringLiteral("한글-NFC.txt"), Nfd = QStringLiteral("한글-NFD.txt").normalized(QString::NormalizationForm_D);
        const auto Mixed = QStringLiteral("한글-") + QStringLiteral("이름").normalized(QString::NormalizationForm_D) + ".txt";
        const auto Folder = QStringLiteral("분리 폴더").normalized(QString::NormalizationForm_D);
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        const QStringList Names{Nfc, Nfd, Mixed, "ASCII.txt"};
        for (const auto& Name : Names) { QFile File(Directory.filePath(Name)); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("unicode"); }
        QVERIFY(QDir(Directory.path()).mkdir(Folder));
        auto* Panel = panels_.front(); Panel->ChangeCurrentPath(Directory.path(), {});
        auto* Model = static_cast<CFSModel*>(model(Panel)); QTRY_VERIFY(!Model->IsLoading() && Model->rowCount() == 6);
        const auto RowOf = [&](const QString& Name) { for (int I = 0; I < Model->rowCount(); ++I) if (Model->GetName(Model->index(I, 0)) == Name) return I; return -1; };
        QMainOpts Dialog(main_); Dialog.show(); QTest::qWait(50);
        auto* Categories = Dialog.findChild<QListWidget*>("lstOptCate"); QVERIFY(Categories);
        for (int I = 0; I < Categories->count(); ++I) if (Categories->item(I)->text() == "파일 그룹 · 색상") Categories->setCurrentRow(I);
        auto* Page = static_cast<hc::ui::FileColorRulesPage*>(Dialog.findChild<QWidget*>("hc.settings.fileColors")); QVERIFY(Page);
        auto Doc = FileColorRules::Empty();
        Doc["groups"] = QJsonArray{QJsonObject{{"id", "unicode"}, {"name", "자소 분리 이름"}, {"enabled", true}, {"join", "all"},
            {"conditions", QJsonArray{QJsonObject{{"type", "extension"}, {"value", "txt"}}}},
            {"light", QJsonObject{{"foreground", "#d15c00"}}}, {"dark", QJsonObject{{"foreground", "#d15c00"}}}, {"font", QJsonObject{{"bold", true}}}}};
        Page->Load(Doc); auto* Table = Page->findChild<QTableWidget*>("hc.rules.conditions"); QVERIFY(Table);
        auto* Field = qobject_cast<QComboBox*>(Table->cellWidget(0, 0)); QVERIFY(Field); QVERIFY(Field->findData("normalization") >= 0);
        Field->setCurrentIndex(Field->findData("normalization"));
        QVERIFY(Table->columnWidth(0) >= Field->fontMetrics().horizontalAdvance(Field->currentText()) + 24);
        QCOMPARE(Page->Document()["version"], QJsonValue(2));
        auto* Choice = Table->cellWidget(0, 2)->findChild<QComboBox*>("hc.condition.value"); QVERIFY(Choice);
        QCOMPARE(Choice->currentData().toString(), QString("decomposed")); QVERIFY(Page->Validate());
        QVERIFY(Dialog.SaveSettings());
        for (const auto& Name : QStringList{Nfc, Nfd, Mixed, "ASCII.txt", Folder}) {
            const int Row = RowOf(Name); QVERIFY(Row >= 0); const bool Decomposed = Name == Nfd || Name == Mixed || Name == Folder;
            const auto Item = Model->GetFileInfo(Model->index(Row, 0));
            QCOMPARE(bool(Item->Flags_ & nsHC::CFileSourceT::FS_FLAG_UNICODE_NFD), Decomposed);
            for (int Col = 0; Col < Model->columnCount(); ++Col) {
                QTRY_COMPARE(Model->index(Row, Col).data(Qt::ForegroundRole).value<QColor>() == QColor("#d15c00"), Decomposed);
                if (Decomposed) QVERIFY(Model->index(Row, Col).data(Qt::FontRole).value<QFont>().bold());
            }
        }
        QVERIFY(SaveR9Dialog(Dialog, "file-color-normalization"));
        Choice->setCurrentIndex(Choice->findData("composed")); QVERIFY(Dialog.SaveSettings());
        QTRY_COMPARE(Model->index(RowOf(Nfc), 0).data(Qt::ForegroundRole).value<QColor>(), QColor("#d15c00"));
        QVERIFY(Model->index(RowOf(Nfd), 0).data(Qt::ForegroundRole).value<QColor>() != QColor("#d15c00"));
        const auto Saved = Page->Document(); Repository Reload(QFileInfo(Settings.FileName()).absolutePath());
        QCOMPARE(Reload.Read(Section::Options).toObject()["fileColorRules"].toObject(), Saved);
        Page->Load(Saved); Choice = Table->cellWidget(0, 2)->findChild<QComboBox*>("hc.condition.value"); QVERIFY(Choice);
        QCOMPARE(Choice->currentData().toString(), QString("composed"));
        for (const auto& Name : Names) { QVERIFY(QFileInfo::exists(Directory.filePath(Name))); QCOMPARE(Model->GetName(Model->index(RowOf(Name), 0)), Name); }
        QVERIFY(QFileInfo(Directory.filePath(Folder)).isDir()); Dialog.reject();
    }

    void fileColorPatternModesUi()
    {
        using namespace hc::settings;
        auto& Settings = AppSettings(); const auto Original = Settings.Object(); const auto Workspace = main_->CaptureWorkspace();
        auto* Colors = TyStColorSchemeMgr::GetInstance();
        const auto Restore = qScopeGuard([&] { Settings.DiscardEdit(); Settings.Replace(Original); Settings.Commit(); Colors->Refresh(); emit Colors->sigColorSchemeChanged(Colors->GetColorScheme(Colors->GetCurrentColorScheme())); main_->RestoreWorkspace(Workspace); });
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        for (int I = 0; I < 400; ++I) { QFile F(Directory.filePath(QString("report_%1.txt").arg(I, 4, 10, QChar('0')))); QVERIFY(F.open(QIODevice::WriteOnly)); F.write("data"); }
        QFile Other(Directory.filePath("notes.dat")); QVERIFY(Other.open(QIODevice::WriteOnly)); Other.write("data"); Other.close();
        auto* Panel = panels_.front(); Panel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {});
        auto* Model = static_cast<CFSModel*>(model(Panel)); QTRY_VERIFY(!Model->IsLoading() && Model->rowCount() >= 401);
        QMainOpts Dialog(main_); Dialog.show(); QTest::qWait(50);
        auto* Page = static_cast<hc::ui::FileColorRulesPage*>(Dialog.findChild<QWidget*>("hc.settings.fileColors")); QVERIFY(Page);
        const QJsonObject Group{{"id", "pattern-modes"}, {"name", "패턴 조건"}, {"join", "all"},
            {"conditions", QJsonArray{QJsonObject{{"type", "extension"}, {"op", "in"}, {"value", "txt"}}}},
            {"light", QJsonObject{{"foreground", "#2266AA"}}}, {"dark", QJsonObject{{"foreground", "#88BBEE"}}}};
        Page->Load({{"version", 2}, {"mode", "first"}, {"groups", QJsonArray{Group}}});
        auto* Table = Page->findChild<QTableWidget*>("hc.rules.conditions");
        auto* Op = qobject_cast<QComboBox*>(Table->cellWidget(0, 1)); QCOMPARE(Op->count(), 6);
        Op->setCurrentIndex(Op->findData("globMatches"));
        Table->cellWidget(0, 2)->findChild<QLineEdit*>("hc.condition.value")->setText("t?t");
        QCOMPARE(Page->Document()["version"], QJsonValue(3)); QVERIFY(Page->Validate()); QVERIFY(Dialog.SaveSettings());
        const auto ExpectedColor = Colors->GetColorScheme(Colors->GetCurrentColorScheme()).IsDarkMode ? QColor("#88BBEE") : QColor("#2266AA");
        // Request more rows than the pending cap once. Completion must retry
        // deferred rows without requiring another user scroll or key event.
        for (int Row = 0; Row < Model->rowCount(); ++Row) Model->index(Row, 0).data(Qt::ForegroundRole);
        QTest::qWait(800);
        const auto AllColored = [&] {
            bool Ready = true;
            // Request every row before yielding, including after an async rule
            // reload. Returning on the first miss would serialize 400 requests
            // at the test polling interval instead of exercising the queue.
            for (int Row = 0; Row < Model->rowCount(); ++Row)
                if (Model->GetName(Model->index(Row, 0)).endsWith(".txt")) Ready &= Model->index(Row, 0).data(Qt::ForegroundRole).value<QColor>() == ExpectedColor;
            return Ready;
        };
        QTRY_VERIFY(AllColored());
        auto* Field = qobject_cast<QComboBox*>(Table->cellWidget(0, 0)); Field->setCurrentIndex(Field->findData("pattern"));
        Op->setCurrentIndex(Op->findData("regexMatches"));
        auto* Value = Table->cellWidget(0, 2)->findChild<QLineEdit*>("hc.condition.value");
        Value->setText("("); QVERIFY(!Page->Validate()); QTRY_VERIFY(!Page->IsValidationPending()); QVERIFY(!Page->Validate());
        Value->setText("\\Areport_\\d{4}\\.txt\\z"); QTRY_VERIFY(!Page->IsValidationPending()); QVERIFY(Page->Validate());
        const auto Saved = Page->Document(); QVERIFY(Dialog.SaveSettings());
        QTRY_VERIFY(AllColored());
        const auto Groups = hc::selection::Groups(Saved); QCOMPARE(Groups.size(), 1); QCOMPARE(Groups[0].version_, 3);
        hc::selection::Criteria Criteria; Criteria.groupId_ = Groups[0].id_; Criteria.group_ = Groups[0].rule_; Criteria.groupVersion_ = Groups[0].version_;
        hc::selection::Matcher Matcher; QVERIFY(Matcher.Compile(Criteria).isEmpty()); QVERIFY(Matcher.Matches(FileRuleInput{"report_0012.txt"})); QVERIFY(!Matcher.Matches(FileRuleInput{"notes.dat"}));
        Repository Reload(QFileInfo(Settings.FileName()).absolutePath()); QCOMPARE(Reload.Read(Section::Options).toObject()["fileColorRules"].toObject(), Saved);
        // Invalidated preview results must not overwrite a newer edit.
        Value->setText("(a+)+$"); Value->setText("\\Anotes\\.dat\\z");
        QTRY_VERIFY(!Page->IsValidationPending()); QVERIFY(Page->Validate());
        if (const auto Out = qEnvironmentVariable("HC_UI_ARTIFACT_DIR"); !Out.isEmpty()) {
            const auto Category = Dialog.findChild<QListWidget*>("lstOptCate");
            for (int I = 0; I < Category->count(); ++I) if (Category->item(I)->text().contains("색상")) Category->setCurrentRow(I);
            QTest::qWait(80); QVERIFY(Dialog.grab().save(QDir(Out).filePath("file-rule-pattern-modes.png")));
        }
        Dialog.reject(); QCOMPARE(Settings.Read(Section::Options).toObject()["fileColorRules"].toObject(), Saved);
    }

    void fileColorFirstPaintUi()
    {
        using namespace hc::settings;
        auto& Settings = AppSettings(); const auto Original = Settings.Object(); const auto Workspace = main_->CaptureWorkspace();
        auto* Colors = TyStColorSchemeMgr::GetInstance();
        const auto Restore = qScopeGuard([&] { Settings.DiscardEdit(); Settings.Replace(Original); Settings.Commit(); Colors->Refresh(); emit Colors->sigColorSchemeChanged(Colors->GetColorScheme(Colors->GetCurrentColorScheme())); main_->RestoreWorkspace(Workspace); });
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        for (int I = 0; I < 120; ++I) { QFile F(Directory.filePath(QString("note_%1.txt").arg(I, 3, 10, QChar('0')))); QVERIFY(F.open(QIODevice::WriteOnly)); F.write("data"); }
        QVERIFY(QDir(Directory.path()).mkdir("nested")); QFile Other(Directory.filePath("other.dat")); QVERIFY(Other.open(QIODevice::WriteOnly)); Other.write("data"); Other.close();
        auto Rules = FileColorRules::Empty(); Rules["version"] = 2;
        Rules["groups"] = QJsonArray{QJsonObject{{"id", "first-paint"}, {"name", "텍스트"}, {"join", "all"},
            {"conditions", QJsonArray{QJsonObject{{"type", "extension"}, {"op", "in"}, {"value", "txt"}}}},
            {"light", QJsonObject{{"foreground", "#2266aa"}}}, {"dark", QJsonObject{{"foreground", "#88bbee"}}}}};
        auto Options = Settings.Read(Section::Options).toObject(); Options["fileColorRules"] = Rules; QVERIFY(Settings.Put(Section::Options, Options));
        auto* Panel = panels_.front(); auto* Model = static_cast<CFSModel*>(model(Panel));
        const auto Scheme = Colors->GetColorScheme(Colors->GetCurrentColorScheme()); Model->SetColorScheme(Scheme);
        const QColor Expected(Scheme.IsDarkMode ? "#88bbee" : "#2266aa");
        const auto StyleUpdates = [](const QSignalSpy& Spy) {
            int Count = 0;
            for (const auto& Signal : Spy) { const auto Roles = Signal[2].value<QList<int>>(); if (Roles.contains(Qt::ForegroundRole) || Roles.contains(CFSModel::FileColorOverrideRole)) ++Count; }
            return Count;
        };
        const auto Painted = [&] {
            // The first read after publishing must already be the group color;
            // a plain color here is the flicker this guards against.
            for (int Row = 0; Row < Model->rowCount(); ++Row) {
                const auto Name = Model->GetName(Model->index(Row, 0)); const auto Color = Model->index(Row, 0).data(Qt::ForegroundRole).value<QColor>();
                if (Name.endsWith(".txt")) { if (Color != Expected) return false; }
                else if (Name != ".." && Color == Expected) return false;
            }
            return true;
        };
        QSignalSpy Entering(Model, &QAbstractItemModel::dataChanged);
        Panel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {}); QTRY_VERIFY(!Model->IsLoading() && Model->rowCount() >= 122);
        QVERIFY(Painted()); QCOMPARE(StyleUpdates(Entering), 0);
        QTest::qWait(150); QVERIFY(Painted()); QCOMPARE(StyleUpdates(Entering), 0);
        // A refresh of the same folder paints from the retained cache as well.
        QSignalSpy Refreshing(Model, &QAbstractItemModel::dataChanged);
        Model->Refresh(); QTRY_VERIFY(!Model->IsLoading() && Model->rowCount() >= 122);
        QVERIFY(Painted()); QCOMPARE(StyleUpdates(Refreshing), 0);
        // Rule or palette changes still clear the cache: a miss returns at
        // once and the row is restyled in a later turn.
        Model->SetColorScheme(Scheme);
        const auto First = [&] { for (int Row = 0; Row < Model->rowCount(); ++Row) if (Model->GetName(Model->index(Row, 0)) == "note_000.txt") return Model->index(Row, 0); return QModelIndex{}; }();
        QVERIFY(First.isValid()); QVERIFY(First.data(Qt::ForegroundRole).value<QColor>() != Expected);
        QTRY_COMPARE(First.data(Qt::ForegroundRole).value<QColor>(), Expected);
        QTRY_VERIFY(Painted());
    }

    void fileColorLargeFolderUi()
    {
        using namespace hc::settings;
        auto& Settings = AppSettings(); const auto Original = Settings.Object(); const auto Workspace = main_->CaptureWorkspace();
        auto* Colors = TyStColorSchemeMgr::GetInstance();
        const auto Restore = qScopeGuard([&] { Settings.DiscardEdit(); Settings.Replace(Original); Settings.Commit(); Colors->Refresh(); emit Colors->sigColorSchemeChanged(Colors->GetColorScheme(Colors->GetCurrentColorScheme())); main_->RestoreWorkspace(Workspace); });
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        // More rows than the worker seeds and than the indexed-order threshold.
        const int Count = 4400;
        for (int I = 0; I < Count; ++I) { QFile F(Directory.filePath(QString("page_%1.txt").arg(I, 5, 10, QChar('0')))); QVERIFY(F.open(QIODevice::WriteOnly)); F.write("d"); }
        auto Rules = FileColorRules::Empty(); Rules["version"] = 2;
        Rules["groups"] = QJsonArray{QJsonObject{{"id", "large-folder"}, {"name", "텍스트"}, {"join", "all"},
            {"conditions", QJsonArray{QJsonObject{{"type", "extension"}, {"op", "in"}, {"value", "txt"}}}},
            {"light", QJsonObject{{"foreground", "#2266aa"}}}, {"dark", QJsonObject{{"foreground", "#88bbee"}}}}};
        auto Options = Settings.Read(Section::Options).toObject(); Options["fileColorRules"] = Rules; QVERIFY(Settings.Put(Section::Options, Options));
        auto* Panel = panels_.front(); auto* Model = static_cast<CFSModel*>(model(Panel));
        const auto Scheme = Colors->GetColorScheme(Colors->GetCurrentColorScheme()); Model->SetColorScheme(Scheme);
        const QColor Expected(Scheme.IsDarkMode ? "#88bbee" : "#2266aa");
        Panel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {}); QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading() && Model->rowCount() >= Count, 30000);
        QVERIFY(Model->UsesIndexedOrder());
        // The background pass styles the rows past the seeds before paging can
        // reach them; every row then paints from the cache on its first read.
        QTRY_VERIFY_WITH_TIMEOUT(Model->FileRuleCompletionIdle(), 10000);
        QSignalSpy Painted(Model, &QAbstractItemModel::dataChanged);
        for (int Row = 0; Row < Model->rowCount(); ++Row) {
            const auto Name = Model->GetName(Model->index(Row, 0)); if (Name == "..") continue;
            QCOMPARE(Model->index(Row, 0).data(Qt::ForegroundRole).value<QColor>(), Expected);
        }
        QTest::qWait(100);
        int StyleUpdates = 0;
        for (const auto& Signal : Painted) { const auto Roles = Signal[2].value<QList<int>>(); if (Roles.contains(Qt::ForegroundRole) || Roles.contains(CFSModel::FileColorOverrideRole)) ++StyleUpdates; }
        QCOMPARE(StyleUpdates, 0);
    }

    void fileColorConditionsUi()
    {
        using namespace hc::settings;
        auto& Settings = AppSettings(); const auto Original = Settings.Object(); const auto Workspace = main_->CaptureWorkspace(); auto* Colors = TyStColorSchemeMgr::GetInstance();
        const auto Restore = qScopeGuard([&] { Settings.DiscardEdit(); Settings.Replace(Original); Settings.Commit(); Colors->Refresh(); emit Colors->sigColorSchemeChanged(Colors->GetColorScheme(Colors->GetCurrentColorScheme())); main_->RestoreWorkspace(Workspace); });
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        for (const auto* Name : {"CMakeLists.txt", "example.cmake", "plain.txt", "launcher.lnk"}) { QFile F(Directory.filePath(Name)); QVERIFY(F.open(QIODevice::WriteOnly)); F.write("data"); QVERIFY(F.setFileTime(QDateTime::currentDateTimeUtc().addSecs(qstrcmp(Name, "plain.txt") == 0 ? -56 : -3600), QFileDevice::FileModificationTime)); }
        panels_.front()->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {}); QTRY_VERIFY(!model(panels_.front())->IsLoading() && names(model(panels_.front())).contains("example.cmake"));
        QMainOpts Dialog(main_); Dialog.show(); QTest::qWait(50); Dialog.findChild<QListWidget*>("lstOptCate")->setCurrentRow(4);
        auto* Page = static_cast<hc::ui::FileColorRulesPage*>(Dialog.findChild<QWidget*>("hc.settings.fileColors")); auto* Table = Page->findChild<QTableWidget*>("hc.rules.conditions"); QCOMPARE(Table->columnCount(), 4);
        auto Old = FileColorRules::Empty(); const QJsonObject LegacyPattern{{"type", "pattern"}, {"value", "literal;*.txt"}, {"futureCondition", "preserve"}};
        Old["groups"] = QJsonArray{QJsonObject{{"id", "conditions-ui"}, {"name", "조건 UI"}, {"enabled", true}, {"join", "any"}, {"conditions", QJsonArray{LegacyPattern}}, {"light", QJsonObject{{"foreground", "#9a3800"}}}, {"dark", QJsonObject{{"foreground", "#ffbd72"}}}}};
        Page->Load(Old); QCOMPARE(Page->Document(), Old);
        Page->findChild<QLineEdit*>("hc.rules.name")->setText("이름만 변경"); QCOMPARE(Page->Document()["version"], QJsonValue(1));
        QCOMPARE(Page->Document()["groups"].toArray()[0].toObject()["conditions"].toArray()[0], QJsonValue(LegacyPattern));
        auto* PatternValue = Table->cellWidget(0, 2)->findChild<QLineEdit*>("hc.condition.value"); QVERIFY(PatternValue->text().contains("\\;"));
        PatternValue->setText("CMakeLists.txt; *.cmake"); QCOMPARE(Page->Document()["version"], QJsonValue(2));
        auto Record = Page->Document()["groups"].toArray()[0].toObject()["conditions"].toArray()[0].toObject(); QCOMPARE(Record["value"].toArray(), QJsonArray({"CMakeLists.txt", "*.cmake"})); QCOMPARE(Record["futureCondition"], QJsonValue("preserve"));
        Page->findChild<QPushButton*>("hc.rules.addCondition")->click();
        auto* Field = qobject_cast<QComboBox*>(Table->cellWidget(1, 0)); Field->setCurrentIndex(Field->findData("size"));
        auto* Operator = qobject_cast<QComboBox*>(Table->cellWidget(1, 1)); QCOMPARE(Operator->currentData().toString(), QString("ge"));
        Table->cellWidget(1, 2)->findChild<QLineEdit*>("hc.condition.value")->setText("4"); auto* Unit = Table->cellWidget(1, 2)->findChild<QComboBox*>("hc.condition.unit"); Unit->setCurrentIndex(Unit->findData("B"));
        Page->findChild<QPushButton*>("hc.rules.all")->click(); QVERIFY(Page->Validate());
        FileColorRules Rules; Rules.Load(Page->Document()); QVERIFY(Rules.Evaluate("example.cmake", 0, 4, false).matched_); QVERIFY(!Rules.Evaluate("example.cmake", 0, 3, false).matched_);
        qobject_cast<QPushButton*>(Table->cellWidget(1, 3))->click(); QCOMPARE(Table->rowCount(), 1); QVERIFY(Page->Validate()); QVERIFY(Dialog.SaveSettings());
        const auto SavedPattern = Page->Document();
        auto* Model = static_cast<CFSModel*>(model(panels_.front())); const auto RowOf = [&](const QString& Name) { for (int I = 0; I < Model->rowCount(); ++I) if (Model->GetName(Model->index(I, 0)) == Name) return I; return -1; };
        QTRY_COMPARE(Model->index(RowOf("example.cmake"), 0).data(Qt::ForegroundRole).value<QColor>(), Colors->GetColorScheme(Colors->GetCurrentColorScheme()).IsDarkMode ? QColor("#ffbd72") : QColor("#9a3800"));
        auto Links = SavedPattern; auto LinkGroups = Links["groups"].toArray(); auto LinkGroup = LinkGroups[0].toObject(); LinkGroup["conditions"] = QJsonArray{QJsonObject{{"type", "link"}, {"op", "is"}, {"value", "any"}}}; LinkGroups[0] = LinkGroup; Links["groups"] = LinkGroups;
        Page->Load(Links); QVERIFY(Dialog.SaveSettings());
        const auto CurrentScheme = Colors->GetColorScheme(Colors->GetCurrentColorScheme()); const auto ExplicitColor = CurrentScheme.IsDarkMode ? QColor("#ffbd72") : QColor("#9a3800");
        QTRY_COMPARE(Model->index(RowOf("launcher.lnk"), 0).data(Qt::ForegroundRole).value<QColor>(), ExplicitColor);
        QTRY_COMPARE(Model->index(RowOf("plain.txt"), 0).data(Qt::ForegroundRole).value<QColor>(), CurrentScheme.FileList_FGColor);
        auto Recent = SavedPattern; auto Groups = Recent["groups"].toArray(); auto Group = Groups[0].toObject();
        Group["conditions"] = QJsonArray{QJsonObject{{"type", "modified"}, {"op", "within"}, {"value", "24"}, {"unit", "hours"}}}; Groups[0] = Group; Recent["groups"] = Groups;
        Page->Load(Recent); QVERIFY(Dialog.SaveSettings()); QTRY_COMPARE(Model->index(RowOf("plain.txt"), 0).data(Qt::ForegroundRole).value<QColor>(), Colors->GetColorScheme(Colors->GetCurrentColorScheme()).IsDarkMode ? QColor("#ffbd72") : QColor("#9a3800"));
        auto Expiring = Recent; auto ExpiringGroups = Expiring["groups"].toArray(); auto ExpiringGroup = ExpiringGroups[0].toObject(); ExpiringGroup["conditions"] = QJsonArray{QJsonObject{{"type", "modified"}, {"op", "within"}, {"value", "1"}, {"unit", "minutes"}}}; ExpiringGroups[0] = ExpiringGroup; Expiring["groups"] = ExpiringGroups;
        Page->Load(Expiring); QVERIFY(Dialog.SaveSettings()); QTRY_COMPARE(Model->index(RowOf("plain.txt"), 0).data(Qt::ForegroundRole).value<QColor>(), ExplicitColor);
        // An idle visible row must lose its relative-date color at expiry;
        // changing no files or settings exercises the model's cache clock.
        QSignalSpy Expiry(Model, &QAbstractItemModel::dataChanged);
        const auto Expired = [&] { for (const auto& Signal : Expiry) if (Signal[2].value<QList<int>>() == QList<int>{Qt::ForegroundRole, Qt::BackgroundRole, Qt::FontRole}
            && Signal[0].value<QModelIndex>().row() <= RowOf("plain.txt") && Signal[1].value<QModelIndex>().row() >= RowOf("plain.txt")) return true; return false; };
        QTRY_VERIFY_WITH_TIMEOUT(Expired(), 8000);
        QTRY_COMPARE(Model->index(RowOf("plain.txt"), 0).data(Qt::ForegroundRole).value<QColor>(), CurrentScheme.FileList_FGColor);
        Page->Load(Recent); QVERIFY(Dialog.SaveSettings());
        const auto Before = Page->Document(); auto* Sample = Page->findChild<QLineEdit*>("hc.rules.sample"); Sample->setText("plain.txt");
        Page->findChild<QCheckBox*>("hc.rules.sampleModifiedKnown")->setChecked(true); Page->findChild<QDateTimeEdit*>("hc.rules.sampleModified")->setDateTime(QDateTime::currentDateTimeUtc().addSecs(-3600));
        Page->findChild<QPushButton*>("hc.rules.addCondition")->click(); auto* LinkField = qobject_cast<QComboBox*>(Table->cellWidget(1, 0)); LinkField->setCurrentIndex(LinkField->findData("link")); Page->findChild<QPushButton*>("hc.rules.any")->click();
        const auto Capture = [&](const QString& Name) { const auto Out = qEnvironmentVariable("HC_UI_ARTIFACT_DIR"); if (Out.isEmpty()) return; QDir().mkpath(Out); QVERIFY(Dialog.grab().save(QDir(Out).filePath(Name + '-' + QString::number(Dialog.devicePixelRatioF()) + ".png"))); };
        for (const bool Dark : {false, true}) { emit Colors->sigColorSchemeChanged(Colors->GetColorScheme(Dark ? QStringLiteral("기본 다크") : QStringLiteral("기본 라이트"))); QCoreApplication::processEvents(); Capture(Dark ? "conditions-dark" : "conditions-light"); }
        QCOMPARE(Table->verticalScrollBar()->maximum(), 0);
        auto Numeric = Before; auto NumericGroups = Numeric["groups"].toArray(); auto NumericGroup = NumericGroups[0].toObject();
        NumericGroup["conditions"] = QJsonArray{QJsonObject{{"type", "size"}, {"op", "ge"}, {"value", "1"}, {"unit", "MiB"}}, QJsonObject{{"type", "attributes"}, {"op", "any"}, {"value", "6"}}, QJsonObject{{"type", "modified"}, {"op", "before"}, {"value", "2026-10-13T12:00:00Z"}}}; NumericGroups[0] = NumericGroup; Numeric["groups"] = NumericGroups;
        Page->Load(Numeric); auto* AttributePicker = Table->cellWidget(1, 2)->findChild<QToolButton*>("hc.condition.attributes"); QVERIFY(AttributePicker);
        AttributePicker->menu()->actions()[1]->setChecked(false);
        QCOMPARE(Page->Document()["groups"].toArray()[0].toObject()["conditions"].toArray()[1].toObject()["value"], QJsonValue("4"));
        AttributePicker->menu()->actions()[1]->setChecked(true); QCOMPARE(Page->Document()["groups"].toArray()[0].toObject()["conditions"].toArray()[1].toObject()["value"], QJsonValue("6"));
        QVERIFY(Page->Validate());
        for (const bool Dark : {false, true}) { emit Colors->sigColorSchemeChanged(Colors->GetColorScheme(Dark ? QStringLiteral("기본 다크") : QStringLiteral("기본 라이트"))); QCoreApplication::processEvents(); Capture(Dark ? "conditions-values-dark" : "conditions-values-light"); }
        QCOMPARE(Table->verticalScrollBar()->maximum(), 0);
        const auto FullSize = Dialog.size(); Dialog.resize(QSize(900, 650)); QCoreApplication::processEvents(); Capture("conditions-small"); Dialog.resize(FullSize); QCoreApplication::processEvents(); QCOMPARE(Dialog.size(), FullSize);
        const auto Size = Dialog.size(); Dialog.move(Dialog.pos() + QPoint(20, 20)); QCoreApplication::processEvents(); QCOMPARE(Dialog.size(), Size);
        Dialog.reject(); QCOMPARE(Settings.Read(Section::Options).toObject()["fileColorRules"].toObject(), Before);
        Repository Restart(QFileInfo(Settings.FileName()).absolutePath()); QCOMPARE(Restart.Read(Section::Options).toObject()["fileColorRules"].toObject(), Before);
    }

    void removedLegacyAppearanceUi()
    {
        using namespace hc::settings;
        auto& Settings = AppSettings(); const auto Original = Settings.Object(); auto* Colors = TyStColorSchemeMgr::GetInstance();
        const auto Restore = qScopeGuard([&] { Settings.DiscardEdit(); Settings.Replace(Original); Settings.Commit(); Colors->Refresh();
            emit Colors->sigColorSchemeChanged(Colors->GetColorScheme(Colors->GetCurrentColorScheme())); });
        auto Appearance = BuiltInAppearance(); auto Schemes = Appearance["schemes"].toObject();
        auto Custom = Schemes[QStringLiteral("기본 라이트")].toObject(); Custom["name"] = "user-custom";
        Custom["futureMetadata"] = "preserve";
        auto Values = Custom["colors"].toObject(); Values["listText"] = "#29462b"; Values["listBackground"] = "#e1e8d7"; Custom["colors"] = Values;
        Custom["fileSets"] = QJsonObject{{"legacy-documents", QJsonObject{{"foreground", "#558800"}, {"background", "#eeeecc"}}}};
        Schemes["user-custom"] = Custom; Appearance["schemes"] = Schemes; Appearance["selected"] = "user-custom";
        QVERIFY(Settings.Put(Section::Appearance, Appearance)); QVERIFY(Settings.Commit()); Colors->Refresh();
        const auto Before = Colors->GetColorScheme("user-custom");
        QMainOpts Dialog(main_); Dialog.show(); QTest::qWait(50);
        QVERIFY(!Dialog.findChild<QWidget*>("pgDisplay_ColorScheme"));
        QCOMPARE(Dialog.findChild<QFontComboBox*>("hc.settings.listFont")->currentFont().family(), Before.FileList_Font.family());
        Dialog.findChild<QSpinBox*>("hc.settings.listFontSize")->setValue(15);
        QVERIFY(Dialog.SaveSettings()); QVERIFY(Dialog.SaveSettings());
        const auto After = Colors->GetColorScheme("user-custom");
        QCOMPARE(After.FileList_Font.pointSize(), 15); QCOMPARE(After.explicitColors_, Before.explicitColors_);
        QCOMPARE(After.FileList_FGColor, Before.FileList_FGColor); QCOMPARE(After.FileList_BGColor, Before.FileList_BGColor);
        QCOMPARE(After.MapNameToColors, Before.MapNameToColors); QCOMPARE(After.Dialog_Font, Before.Dialog_Font); QCOMPARE(After.Menu_Font, Before.Menu_Font);
        Repository Restart(QFileInfo(Settings.FileName()).absolutePath()); const auto Saved = Restart.Read(Section::Appearance).toObject();
        QCOMPARE(Saved["selected"].toString(), QString("user-custom"));
        QCOMPARE(Saved["schemes"].toObject()["user-custom"].toObject()["futureMetadata"].toString(), QString("preserve"));
        Dialog.findChild<QSpinBox*>("hc.settings.listFontSize")->setValue(17); Dialog.reject();
        QCOMPARE(Colors->GetColorScheme("user-custom").FileList_Font.pointSize(), 15);
    }

    void todoBugFocusFrameFontDpi()
    {
        const auto Workspace = main_->CaptureWorkspace(); auto* Colors = TyStColorSchemeMgr::GetInstance();
        const auto Original = Colors->GetColorScheme(Colors->GetCurrentColorScheme());
        const auto Restore = qScopeGuard([&] { emit Colors->sigColorSchemeChanged(Original); main_->RestoreWorkspace(Workspace); });
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        QFile File(Directory.filePath("focus.txt")); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("frame"); File.close();
        auto* Panel = panels_.front(); Panel->ChangeCurrentPath(Directory.path(), {});
        QTRY_VERIFY(!model(Panel)->IsLoading() && model(Panel)->rowCount() == 2);
        auto* View = static_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex()));
        QVERIFY(View->SetListMode("one-line")); View->RestoreSelection({}, {}); View->SetPanelActive(true); View->BaseView()->setFocusedRowIndex(1);
        for (const int Points : {10, 13, 16, 17, 20}) {
            auto Scheme = Original; Scheme.FileList_Font = QFont("D2Coding ligature", Points); Scheme.IsInverseCursor = false; Scheme.IsInverseSelect = false;
            emit Colors->sigColorSchemeChanged(Scheme); QTest::qWait(120);
            const int Height = View->RecordGeometry().pitch;
            for (const qreal Dpr : {1.0, 1.25, 1.5, 1.75, 2.0}) for (int Position = 0; Position < 4; ++Position) {
                QImage Image(qCeil(430 * Dpr), qCeil((Height * 5 + 12) * Dpr), QImage::Format_ARGB32_Premultiplied); Image.setDevicePixelRatio(Dpr); Image.fill(Qt::transparent);
                QPainter Painter(&Image); Painter.translate(3, 3 + Position * Height);
                Qtitan::GridRecordPainter::Record Record; Record.rect = QRect(0, 0, 420, Height); Record.rowIndex = 1; Record.index = View->BaseView()->getRow(1).modelIndex(0);
                const auto Bounds = Painter.deviceTransform().mapRect(QRectF(Record.rect)).toAlignedRect();
                Painter.setClipRect(Record.rect);
                View->BaseView()->recordPainter()->paintOverlay(Painter, Record, View); Painter.end();
                const auto Ink = [&](QRect Rect) { int Count = 0; Rect = Rect.intersected(Image.rect());
                    for (int Y = Rect.top(); Y <= Rect.bottom(); ++Y) for (int X = Rect.left(); X <= Rect.right(); ++X) if (Image.pixelColor(X, Y).alpha() > 100) ++Count; return Count; };
                const QString Context = QString("font=%1pt DPR=%2 row=%3 height=%4").arg(Points).arg(Dpr).arg(Position).arg(Height);
                QVERIFY2(Ink(QRect(Bounds.left(), Bounds.top() + 4, 2, Bounds.height() - 8)) > 0, qPrintable("Left edge missing: " + Context));
                QVERIFY2(Ink(QRect(Bounds.right() - 1, Bounds.top() + 4, 2, Bounds.height() - 8)) > 0, qPrintable("Right edge missing: " + Context));
                QVERIFY2(Ink(QRect(Bounds.left() + 4, Bounds.top(), Bounds.width() - 8, 2)) > 0, qPrintable("Top edge missing: " + Context));
                QVERIFY2(Ink(QRect(Bounds.left() + 4, Bounds.bottom() - 1, Bounds.width() - 8, 2)) > 0, qPrintable("Bottom edge missing: " + Context));
            }
        }
    }

    void todoBugFileStylesAllColumns()
    {
        using namespace hc::settings;
        auto& Settings = AppSettings(); const auto Before = Settings.Object(); const auto Workspace = main_->CaptureWorkspace();
        auto* Colors = TyStColorSchemeMgr::GetInstance(); const auto OriginalScheme = Colors->GetColorScheme(Colors->GetCurrentColorScheme());
        const auto Restore = qScopeGuard([&] { Settings.Replace(Before); Settings.Commit(); emit Colors->sigColorSchemeChanged(OriginalScheme); main_->RestoreWorkspace(Workspace); });
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        QFile File(Directory.filePath("row-style.txt")); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("all columns"); File.close();
        auto* Panel = panels_.front(); Panel->ChangeCurrentPath(Directory.path(), {});
        QTRY_VERIFY(!model(Panel)->IsLoading() && names(model(Panel)).contains("row-style.txt"));
        auto* View = static_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex()));
        QVERIFY(View->ApplyColumnProfile(hc::columns::ReferenceProfile()));
        auto* Model = static_cast<CFSModel*>(model(Panel));
        const auto Paint = [&](const QModelIndex& Index) {
            auto* Column = static_cast<Qtitan::GridTableColumn*>(View->BaseView()->getColumnByModelColumn(Index.column()));
            auto* Repository = static_cast<Qtitan::GridDelegateAdapterEditorRepository*>(Column->editorRepository());
            QImage Image(420, 64, QImage::Format_ARGB32_Premultiplied); Image.fill(Qt::transparent);
            QPainter Painter(&Image); QStyleOptionViewItem Option; Option.rect = Image.rect(); Option.font = View->font();
            const auto Cell = View->BaseView()->getRow(Index.row()).modelIndex(Index.column());
            Repository->getDelegate()->paint(&Painter, Option, Cell); Painter.end(); return Image;
        };
        const auto HasColor = [](const QImage& Image, const QColor& Color) {
            for (int Y = 0; Y < Image.height(); ++Y) for (int X = 0; X < Image.width(); ++X) {
                const auto Pixel = Image.pixelColor(X, Y);
                if (Pixel.alpha() > 200 && qAbs(Pixel.red() - Color.red()) < 12 && qAbs(Pixel.green() - Color.green()) < 12 && qAbs(Pixel.blue() - Color.blue()) < 12) return true;
            }
            return false;
        };
        for (const bool Dark : {false, true}) for (const auto* Mode : {"one-line", "two-line"}) {
            auto Scheme = Colors->GetColorScheme(Dark ? QStringLiteral("기본 다크") : QStringLiteral("기본 라이트"));
            const QColor Foreground(Dark ? "#48ed73" : "#9434b0");
            QVERIFY(View->SetListMode(Mode)); View->RestoreSelection({}, {}); View->BaseView()->setFocusedRowIndex(0);
            QVector<QImage> Plain;
            for (const bool Bold : {false, true}) {
                auto Rules = FileColorRules::Empty(); Rules["groups"] = QJsonArray{QJsonObject{{"id", "row-style"}, {"name", "행 스타일"}, {"enabled", true}, {"join", "any"},
                    {"conditions", QJsonArray{QJsonObject{{"type", "extension"}, {"value", "txt"}}}},
                    {"light", QJsonObject{{"foreground", "#9434b0"}}}, {"dark", QJsonObject{{"foreground", "#48ed73"}}}, {"font", QJsonObject{{"bold", Bold}}}}};
                auto Options = Settings.Read(Section::Options).toObject(); Options["fileColorRules"] = Rules; QVERIFY(Settings.Put(Section::Options, Options));
                emit Colors->sigColorSchemeChanged(Scheme); QTest::qWait(100);
                int Row = -1; for (int I = 0; I < Model->rowCount(); ++I) if (Model->GetName(Model->index(I, 0)) == "row-style.txt") Row = I;
                QVERIFY(Row >= 0); QVERIFY(Model->columnCount() >= 5);
                for (int Column = 0; Column < Model->columnCount(); ++Column) {
                    const auto Index = Model->index(Row, Column); QVERIFY(!Index.data().toString().isEmpty());
                    QCOMPARE(Index.data(Qt::ForegroundRole).value<QColor>(), Foreground); QCOMPARE(Index.data(Qt::FontRole).value<QFont>().bold(), Bold);
                    const auto Image = Paint(Index); QVERIFY2(HasColor(Image, Foreground), qPrintable(QString("Rule color missing in column %1 (%2)").arg(Column).arg(Index.data().toString())));
                    if (Bold) { if (Column > 0) QVERIFY2(Image != Plain[Column], qPrintable(QString("Bold has no visual effect in column %1").arg(Column))); } else Plain.append(Image);
                }
                Scheme.IsInverseCursor = true; emit Colors->sigColorSchemeChanged(Scheme); QTest::qWait(100);
                View->SetPanelActive(true); View->BaseView()->setFocusedRowIndex(Row);
                QVERIFY(View->ListAppearance().invertCursor);
                for (int Column = 0; Column < Model->columnCount(); ++Column)
                    QVERIFY2(HasColor(Paint(Model->index(Row, Column)), Foreground), qPrintable(QString("Rule color missing at inverted cursor in column %1").arg(Column)));
                Scheme.IsInverseCursor = false; emit Colors->sigColorSchemeChanged(Scheme); QTest::qWait(100);
                View->BaseView()->setFocusedRowIndex(0);
                if (Bold) {
                    const auto Key = hc::workspace::SelectionKey(*Model->GetFileInfo(Model->index(Row, 0))->storageEntry_);
                    View->RestoreSelection({Key}, Key);
                    for (int Column = 0; Column < Model->columnCount(); ++Column) QVERIFY(!HasColor(Paint(Model->index(Row, Column)), Foreground));
                    View->RestoreSelection({}, {});
                }
            }
            const auto Out = qEnvironmentVariable("HC_UI_ARTIFACT_DIR"); if (!Out.isEmpty()) QVERIFY(main_->grab().save(QDir(Out).filePath(QString("all-columns-%1-%2.png").arg(Dark ? "dark" : "light", Mode))));
        }
    }

    void fileColorSettingsUi()
    {
        auto* ProductStyle = qobject_cast<QProxyStyle*>(qApp->style());
        QVERIFY(ProductStyle && qobject_cast<fm::style::WatercolorStyle*>(ProductStyle->baseStyle()));
        using namespace hc::settings;
        auto& Settings = AppSettings(); const auto Original = Settings.Object(); const auto Workspace = main_->CaptureWorkspace();
        auto* Colors = TyStColorSchemeMgr::GetInstance();
        const auto Restore = qScopeGuard([&] { Settings.DiscardEdit(); Settings.Replace(Original); Settings.Commit(); Colors->Refresh();
            emit Colors->sigColorSchemeChanged(Colors->GetColorScheme(Colors->GetCurrentColorScheme())); main_->RestoreWorkspace(Workspace); });
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        for (const auto* Name : {"sample.txt", "UPPER.TXT", "photo.jpg", "archive.tar.gz", "README"}) { QFile File(Directory.filePath(Name)); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("p4 fixture"); }
        QVERIFY(QDir(Directory.path()).mkdir("folder.txt"));
        for (auto* Panel : panels_) { Panel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {}); QTRY_VERIFY(!model(Panel)->IsLoading() && names(model(Panel)).contains("sample.txt")); }
        auto* View = qobject_cast<CGridView*>(panels_.front()->GetFocusView(panels_.front()->CurrentTabIndex()));
        QMainOpts Dialog(main_); Dialog.SetListView(View); Dialog.show(); QTest::qWait(50); Dialog.LoadSettings();
        auto* Page = static_cast<hc::ui::FileColorRulesPage*>(Dialog.findChild<QWidget*>("hc.settings.fileColors")); QVERIFY(Page);
        QVERIFY(!Dialog.findChild<QWidget*>("pgDisplay_ColorScheme"));
        QVERIFY(!Dialog.findChild<QComboBox*>("cbxColorScheme"));
        for (auto* Button : Dialog.findChildren<QPushButton*>()) QVERIFY(!Button->text().contains("기존 색 구성표"));
        for (const auto& Name : {"hc.rules.groupsCard", "hc.rules.definitionCard", "hc.rules.appearanceCard", "hc.rules.previewCard"}) QVERIFY(Page->findChild<fm::ui::Card*>(Name));
        const auto InitialSize = Dialog.size();
        if (InitialSize.height() > 850) {
            auto* Stack = Dialog.findChild<QStackedWidget*>("stkOpts");
            for (int I = 0; I < Stack->count(); ++I) { auto* Widget = Stack->widget(I); qInfo() << "Settings page minimum" << I << Widget->objectName() << Widget->minimumSizeHint() << Widget->minimumSize() << Widget->heightForWidth(Widget->width()); }
        }
        QVERIFY2(InitialSize.height() <= 850, qPrintable(QString::number(InitialSize.height())));
        Dialog.findChild<QListWidget*>("lstOptCate")->setCurrentRow(4);
        QCoreApplication::processEvents();
        for (int Offset : {20, -20, 20, -20}) {
            Dialog.move(Dialog.pos() + QPoint(Offset, Offset)); QTest::qWait(30);
            QCOMPARE(Dialog.size(), InitialSize);
        }
        Dialog.resize(InitialSize + QSize(0, 120)); QCoreApplication::processEvents();
        Dialog.resize(InitialSize); QCoreApplication::processEvents();
        QCOMPARE(Dialog.size(), InitialSize);
        auto Doc = FileColorRules::Empty(); Doc["groups"] = QJsonArray{QJsonObject{{"id", "p4-documents"}, {"name", "문서"}, {"enabled", true}, {"join", "any"},
            {"conditions", QJsonArray{QJsonObject{{"type", "extension"}, {"value", "txt"}}}}, {"light", QJsonObject{{"foreground", "#9a3800"}}},
            {"dark", QJsonObject{{"foreground", "#ffbd72"}}}, {"font", QJsonObject{{"bold", true}}}}};
        Page->Load(Doc);
        auto* GroupList = Page->findChild<fm::ui::ReorderListWidget*>("hc.rules.groups");
        QVERIFY(GroupList->item(0)->text().contains("1. 문서\n확장자: txt"));
        QVERIFY(!GroupList->item(0)->icon().isNull());
        Page->findChild<QRadioButton*>("hc.rules.merge")->click(); QCOMPARE(Page->Document()["mode"].toString(), QString("merge"));
        Page->findChild<QRadioButton*>("hc.rules.first")->click(); QCOMPARE(Page->Document(), Doc);
        QVERIFY(Dialog.SaveSettings());
        const auto Save = [&](QWidget* Widget, const QString& Name) { const auto Out = qEnvironmentVariable("HC_UI_ARTIFACT_DIR"); if (Out.isEmpty()) return; QDir().mkpath(Out); QVERIFY(Widget->grab().save(QDir(Out).filePath(Name + '-' + QString::number(Widget->devicePixelRatioF()) + ".png"))); };
        for (const bool Dark : {false, true}) {
            auto Scheme = Colors->GetColorScheme(Dark ? QStringLiteral("기본 다크") : QStringLiteral("기본 라이트")); emit Colors->sigColorSchemeChanged(Scheme);
            for (auto* Panel : panels_) {
                auto* Model = static_cast<CFSModel*>(model(Panel));
                for (int Row = 0; Row < Model->rowCount(); ++Row) {
                    const auto Index = Model->index(Row, 0); const auto Name = Model->GetName(Index);
                    // A palette change clears the style cache; rows restyle in a later turn.
                    if (Name == "sample.txt" || Name == "UPPER.TXT") { QTRY_COMPARE(Model->data(Index, Qt::ForegroundRole).value<QColor>(), QColor(Dark ? "#ffbd72" : "#9a3800")); QVERIFY(Model->data(Index, Qt::FontRole).value<QFont>().bold()); }
                    if (Name == "folder.txt" || Name == "photo.jpg") QTRY_COMPARE(Model->data(Index, Qt::ForegroundRole).value<QColor>(), Scheme.FileList_FGColor);
                }
            }
            Dialog.hide(); QCoreApplication::processEvents(); Save(main_, Dark ? "p4-panels-dark" : "p4-panels-light"); Dialog.show();
            auto* Categories = Dialog.findChild<QListWidget*>("lstOptCate");
            QCOMPARE(Categories->palette().color(QPalette::Base), fm::style::ThemeManager::instance().colors().toPalette().color(QPalette::Base));
            for (int Row = 0; Row < 8; ++Row) { Categories->setCurrentRow(Row); QCoreApplication::processEvents(); Save(&Dialog, QString("p4-page-%1-%2").arg(Row).arg(Dark ? "dark" : "light")); }
        }
        Repository Reload(QFileInfo(Settings.FileName()).absolutePath()); QCOMPARE(Reload.Read(Section::Options).toObject()["fileColorRules"].toObject(), Doc);
        auto Changed = Doc; auto Groups = Changed["groups"].toArray(); auto Group = Groups[0].toObject(); Group["name"] = "취소할 이름"; Groups[0] = Group; Changed["groups"] = Groups;
        Page->Load(Changed); Dialog.reject();
        QCOMPARE(Settings.Read(Section::Options).toObject()["fileColorRules"].toObject(), Doc);
    }
    void r9FileMasksUi()
    {
        using namespace hc::ui; QTemporaryDir Directory; QVERIFY(Directory.isValid());
        const auto Before = hc::settings::AppSettings().Object(); const auto Restore = qScopeGuard([&] { hc::settings::AppSettings().Replace(Before); hc::settings::AppSettings().Commit(); });
        auto Exif = QByteArray::fromHex("49492a0008000000010069870400010000001a00000000000000010003900200140000002c00000000000000") + QByteArray("2024:02:29 12:34:56\0",20);
        QStringList Paths;
        for (const auto& Pair : {qMakePair(QString::fromUtf8("가😀나다.TXT"),QByteArray("text")),qMakePair(QString("photo.tif"),Exif)}) {
            QFile File(Directory.filePath(Pair.first)); QVERIFY(File.open(QIODevice::WriteOnly)); File.write(Pair.second); Paths.append(File.fileName());
        }
        BatchRenameDialog Dialog(Paths,true,main_); Dialog.show(); auto* Mask = Dialog.findChild<QLineEdit*>("hc.rename.mask");
        QTRY_VERIFY(Mask->isEnabled()); Mask->setText("[N:1:2]_[C:+5]"); QTRY_VERIFY(Dialog.AcceptButton()->isEnabled());
        QVERIFY(SaveR9Dialog(Dialog,"r9-rename-masks"));
        Dialog.findChild<QLineEdit*>("hc.rename.find")->setText(".TXT"); Dialog.findChild<QLineEdit*>("hc.rename.replace")->setText(".md"); Dialog.findChild<QCheckBox*>("hc.rename.includeExtension")->setChecked(true);
        QTRY_VERIFY(Dialog.AcceptButton()->isEnabled()); QVERIFY(SaveR9Dialog(Dialog,"r9-rename-extension"));
        Mask->setText("[date]_[C]"); Dialog.findChild<QComboBox*>("hc.rename.dateSource")->setCurrentIndex(1);
        QVERIFY(!Dialog.AcceptButton()->isEnabled()); QTRY_VERIFY(Dialog.findChild<QLabel*>("hc.rename.summary")->text().contains("충돌/오류 1"));
        QVERIFY(!Dialog.AcceptButton()->isEnabled()); QVERIFY(SaveR9Dialog(Dialog,"r9-rename-exif-missing"));
        Dialog.findChild<QComboBox*>("hc.rename.dateSource")->setCurrentIndex(0); Mask->setText("[N]_[C]"); QTRY_VERIFY(Dialog.AcceptButton()->isEnabled());
        Mask->setText("[N1"); QTRY_VERIFY(Dialog.findChild<QLabel*>("hc.rename.summary")->text().contains("충돌/오류 2")); QVERIFY(!Dialog.AcceptButton()->isEnabled());
        Dialog.CancelButton()->click();
        BatchRenameDialog Photo({Paths.last()},true,main_); Photo.show(); QTRY_VERIFY(Photo.findChild<QLineEdit*>("hc.rename.mask")->isEnabled());
        Photo.findChild<QLineEdit*>("hc.rename.mask")->setText("[date]_[C]"); Photo.findChild<QLineEdit*>("hc.rename.date")->setText("yyyyMMdd_HHmmss");
        Photo.findChild<QSpinBox*>("hc.rename.dateOffset")->setValue(1); Photo.findChild<QComboBox*>("hc.rename.dateSource")->setCurrentIndex(1);
        QTRY_VERIFY(Photo.AcceptButton()->isEnabled());
        auto* Preview = Photo.findChild<QTableWidget*>("hc.rename.preview"); QCOMPARE(Preview->horizontalHeaderItem(3)->text(),QString("촬영 날짜")); QVERIFY(Preview->item(0,3)->text().contains("2024"));
        QVERIFY(SaveR9Dialog(Photo,"r9-rename-exif")); Photo.AcceptButton()->click();
        QTRY_VERIFY(Photo.findChild<QPushButton*>("hc.rename.undo")->isEnabled()); QVERIFY(QFileInfo::exists(Directory.filePath("20240229_123457_001.tif")));
        Photo.findChild<QComboBox*>("hc.rename.dateSource")->setCurrentIndex(0); Photo.findChild<QComboBox*>("hc.rename.dateSource")->setCurrentIndex(1);
        QVERIFY(!Photo.findChild<QPushButton*>("hc.rename.undo")->isEnabled()); QTRY_VERIFY(Photo.findChild<QPushButton*>("hc.rename.undo")->isEnabled());
        Photo.findChild<QPushButton*>("hc.rename.undo")->click(); QTRY_VERIFY(Photo.findChild<QPushButton*>("hc.rename.redo")->isEnabled()); QVERIFY(QFileInfo::exists(Paths.last())); Photo.CancelButton()->click();
        // Closing during a pending metadata read must discard its completion.
        auto* Closed = new BatchRenameDialog(Paths,true,main_); Closed->show(); QTRY_VERIFY(Closed->findChild<QLineEdit*>("hc.rename.mask")->isEnabled());
        Closed->findChild<QComboBox*>("hc.rename.dateSource")->setCurrentIndex(1); delete Closed; QTest::qWait(150);
    }
    void r15RenameQueueUi()
    {
        using namespace hc::ui; using namespace hc::filetools; using namespace hc::operations;
        auto* Jobs = dynamic_cast<FileJobUi*>(hc::permissions::Controller()); QVERIFY(Jobs);
        QTemporaryDir Directory; QVERIFY(Directory.isValid()); QStringList Paths;
        for (int I = 0; I < 3; ++I) { QFile File(Directory.filePath(QString("source%1.txt").arg(I))); QVERIFY(File.open(QIODevice::WriteOnly)); File.write(QByteArray::number(I)); Paths.append(File.fileName()); }
        const auto Before = Jobs->Scheduler().Snapshots(); QSet<QString> Existing; for (const auto& Job : Before) Existing.insert(QString::fromStdString(Job.id_));
        BatchRenameDialog Dialog(Paths, true, main_, {}, Jobs); Dialog.show(); auto* Mask = Dialog.findChild<QLineEdit*>("hc.rename.mask"); QTRY_VERIFY(Mask->isEnabled());
        Mask->setText("renamed_[C]"); QTRY_VERIFY(Dialog.AcceptButton()->isEnabled()); QVERIFY(SaveR9Dialog(Dialog,"r15-rename-plan"));
        Dialog.findChild<QCheckBox*>("hc.rename.queued")->setChecked(true); Dialog.AcceptButton()->click(); QTRY_VERIFY(!Dialog.isVisible());
        std::string JobId; QString RecordId;
        for (const auto& Job : Jobs->Scheduler().Snapshots()) if (!Existing.contains(QString::fromStdString(Job.id_)) && Job.metadata_.options_.starts_with("rename-record=")) { JobId = Job.id_; RecordId = QString::fromStdString(Job.metadata_.options_).mid(14); }
        QVERIFY(!JobId.empty()); QVERIFY(Jobs->Scheduler().Snapshot(JobId).Value().deferred_); for (const auto& Path : Paths) QVERIFY(QFileInfo::exists(Path));
        Jobs->Show(); auto* Manager = main_->findChild<QDialog*>("hc.jobManager"); QVERIFY(Manager); QVERIFY(SaveR9Dialog(*Manager,"r15-rename-queued"));
        QVERIFY(Jobs->Scheduler().Control({JobId}, JobControl::Start).HasValue()); QTRY_COMPARE_WITH_TIMEOUT(Jobs->Scheduler().Snapshot(JobId).Value().state_, JobState::Completed, 30000);
        QTRY_VERIFY(!Jobs->IsUiPending(JobId)); Manager->hide();
        for (const auto& Path : Paths) QVERIFY(!QFileInfo::exists(Path));
        RenameHistoryDialog History(Jobs, main_); History.show(); auto* List = History.findChild<QTreeWidget*>("hc.renameHistory.list"); QVERIFY(List);
        QTreeWidgetItem* Item = nullptr; QTRY_VERIFY_WITH_TIMEOUT(([&] { for (int I = 0; I < List->topLevelItemCount(); ++I) if (List->topLevelItem(I)->data(0,Qt::UserRole).toString() == RecordId) { Item = List->topLevelItem(I); return true; } return false; })(), 15000);
        List->setCurrentItem(Item); auto* Undo = History.findChild<QPushButton*>("hc.renameHistory.undo"); QTRY_VERIFY(Undo->isEnabled()); QVERIFY(SaveR9Dialog(History,"r15-rename-history")); History.CancelButton()->click();
        // A fresh product process loads the history and submits undo through its
        // own shared scheduler. The parent waits while no rename worker is live.
        QProcess Restart; auto Environment = QProcessEnvironment::systemEnvironment(); Environment.insert("HC_R15_RENAME_RECORD", RecordId); Environment.insert("HC_R15_RENAME_FOLDER", Directory.path());
        Environment.insert("HC_TEST_CONFIG_DIR", QFileInfo(hc::settings::AppSettings().FileName()).absolutePath());
        Restart.setProcessEnvironment(Environment); Restart.start(QCoreApplication::applicationFilePath(), {"r15RenameRestartUi", "-o", "r15-rename-restart.txt,txt"});
        QVERIFY(Restart.waitForStarted(5000)); QTRY_VERIFY_WITH_TIMEOUT(Restart.state() == QProcess::NotRunning, 30000); QCOMPARE(Restart.exitCode(), 0);
        for (const auto& Path : Paths) QVERIFY(QFileInfo::exists(Path));
        // Detaching the editor must not cancel the scheduler-owned job.
        BatchRenameDialog Background(Paths, true, main_, {}, Jobs); Background.show(); auto* BackgroundMask = Background.findChild<QLineEdit*>("hc.rename.mask"); QTRY_VERIFY(BackgroundMask->isEnabled());
        BackgroundMask->setText("background_[C]"); QTRY_VERIFY(Background.AcceptButton()->isEnabled()); Background.AcceptButton()->click();
        auto* Hide = Background.findChild<QPushButton*>("hc.rename.background"); QTRY_VERIFY(Hide->isEnabled()); Hide->click(); QVERIFY(!Background.isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(Directory.filePath("background_003.txt")), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(([&] { for (const auto& Snapshot : Jobs->Scheduler().Snapshots()) if (Snapshot.metadata_.source_ == Directory.path().toStdString() && !IsTerminal(Snapshot.state_)) return false; return true; })(), 15000);
        // Reproduce an interrupted temp rename without finalizing the task. The
        // separate core suite also forces real child-process exit at both phases.
        QTemporaryDir Interrupted; QList<RenameRow> RecoveryRows;
        for (int I = 0; I < 2; ++I) {
            QFile File(Interrupted.filePath(QString("original%1.txt").arg(I))); QVERIFY(File.open(QIODevice::WriteOnly)); File.write(QByteArray::number(I)); File.close();
            RecoveryRows.append({Inspect(File.fileName()), Interrupted.filePath(QString("target%1.txt").arg(I)), {}});
        }
        auto Prepared = PrepareBatchRename(RenameJournalDirectory(), RecoveryRows); QVERIFY(Prepared.HasValue());
        const auto RecoveryId = QString::fromStdString(Prepared.Value().request_.metadata_.options_).mid(14);
        auto Record = ReadRenameRecord(RenameJournalDirectory(), RecoveryId); QVERIFY(Record.HasValue());
        auto Task = std::move(Prepared.Value().task_); JobStepContext Context; Context.jobId_ = "hc-job-ui-interruption";
        for (int I = 0; I < 20; ++I) { const auto Step = Task->Advance(Context); Context.committing_ = Step.next_ == JobState::Committing; if (QFileInfo::exists(Record.Value().temporary_[0])) break; }
        QVERIFY(QFileInfo::exists(Record.Value().temporary_[0])); Task.reset();
        RenameHistoryDialog RecoveryHistory(Jobs, main_); RecoveryHistory.show(); auto* RecoveryList = RecoveryHistory.findChild<QTreeWidget*>("hc.renameHistory.list"); QTreeWidgetItem* RecoveryItem = nullptr;
        QTRY_VERIFY(([&] { for (int I = 0; I < RecoveryList->topLevelItemCount(); ++I) if (RecoveryList->topLevelItem(I)->data(0,Qt::UserRole).toString() == RecoveryId) { RecoveryItem = RecoveryList->topLevelItem(I); return true; } return false; })());
        RecoveryList->setCurrentItem(RecoveryItem); auto* RestoreButton = RecoveryHistory.findChild<QPushButton*>("hc.renameHistory.restore"); QTRY_VERIFY(RestoreButton->isEnabled());
        QVERIFY(RecoveryHistory.findChild<QPushButton*>("hc.renameHistory.resume")->isEnabled()); QVERIFY(SaveR9Dialog(RecoveryHistory,"r15-rename-interrupted")); RestoreButton->click();
        QTRY_COMPARE_WITH_TIMEOUT(ReadRenameRecord(RenameJournalDirectory(), RecoveryId).Value().state_, RenameState::Restored, 15000);
        for (const auto& Row : RecoveryRows) QVERIFY(QFileInfo::exists(Row.source_.path_));
        QTRY_VERIFY_WITH_TIMEOUT(([&] { for (const auto& Snapshot : Jobs->Scheduler().Snapshots()) if (Snapshot.metadata_.source_ == Interrupted.path().toStdString() && !IsTerminal(Snapshot.state_)) return false; return true; })(), 15000);
        QTest::qWait(250); QVERIFY(SaveR9Dialog(RecoveryHistory,"r15-rename-restored")); RecoveryHistory.CancelButton()->click();
    }
    void r15RenameRestartUi()
    {
        using namespace hc::ui;
        const auto Id = qEnvironmentVariable("HC_R15_RENAME_RECORD"); if (Id.isEmpty()) QSKIP("Parent rename history fixture required");
        auto* Jobs = dynamic_cast<FileJobUi*>(hc::permissions::Controller()); QVERIFY(Jobs); RenameHistoryDialog History(Jobs, main_); History.show();
        auto* List = History.findChild<QTreeWidget*>("hc.renameHistory.list"); QTreeWidgetItem* Item = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT(([&] { for (int I = 0; I < List->topLevelItemCount(); ++I) if (List->topLevelItem(I)->data(0,Qt::UserRole).toString() == Id) { Item = List->topLevelItem(I); return true; } return false; })(), 15000);
        List->setCurrentItem(Item); auto* Undo = History.findChild<QPushButton*>("hc.renameHistory.undo"); QTRY_VERIFY(Undo->isEnabled()); QVERIFY(SaveR9Dialog(History,"r15-rename-restart-history")); Undo->click();
        const auto Folder = qEnvironmentVariable("HC_R15_RENAME_FOLDER"); QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(QDir(Folder).filePath("source2.txt")), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(([&] { for (const auto& Snapshot : Jobs->Scheduler().Snapshots()) if (Snapshot.metadata_.source_ == Folder.toStdString() && !hc::operations::IsTerminal(Snapshot.state_)) return false; return true; })(), 15000);
        History.CancelButton()->click();
    }
    void r9LargeRenameUi()
    {
        using namespace hc::ui; QTemporaryDir Directory; QVERIFY(Directory.isValid());
        const auto Before = hc::settings::AppSettings().Object(); const auto Restore = qScopeGuard([&] { hc::settings::AppSettings().Replace(Before); hc::settings::AppSettings().Commit(); });
        auto Manifest = std::make_shared<R9Manifest>(); Manifest->provider_ = hc::storage::MakeNativeReadProvider(hc::storage::LocatorKind::Local);
        for (int I = 0; I < 2501; ++I) {
            QFile File(Directory.filePath(QString("source_%1.txt").arg(I,4,10,QChar('0')))); QVERIFY(File.open(QIODevice::WriteOnly)); File.write(QByteArray::number(I)); File.close();
            auto Entry = Manifest->provider_->Resolve({hc::storage::LocatorKind::Local,File.fileName().toStdString()},{}).get(); QVERIFY(Entry.HasValue()); Manifest->entries_.push_back(Entry.Value());
        }
        BatchRenameDialog Dialog({},true,main_,Manifest); Dialog.show(); auto* Mask = Dialog.findChild<QLineEdit*>("hc.rename.mask"); QTRY_VERIFY_WITH_TIMEOUT(Mask->isEnabled(),30000);
        Mask->setText("renamed_[C]"); QTRY_VERIFY_WITH_TIMEOUT(Dialog.AcceptButton()->isEnabled(),30000);
        QCOMPARE(Manifest->maxRead_.load(),std::size_t(256)); QCOMPARE(Dialog.findChild<QTableWidget*>("hc.rename.preview")->rowCount(),256);
        QVERIFY(Dialog.findChild<QLabel*>("hc.rename.summary")->text().startsWith("2501개 중 변경 2501")); QVERIFY(SaveR9Dialog(Dialog,"r9-rename-large-first"));
        auto* Next = Dialog.findChild<QPushButton*>("hc.rename.nextPage"); for (int Page = 0; Page < 9; ++Page) { QVERIFY(Next->isEnabled()); Next->click(); }
        QCOMPARE(Dialog.findChild<QTableWidget*>("hc.rename.preview")->rowCount(),197); QVERIFY(!Next->isEnabled()); QVERIFY(SaveR9Dialog(Dialog,"r9-rename-large-last"));
        Dialog.AcceptButton()->click(); auto* Undo = Dialog.findChild<QPushButton*>("hc.rename.undo"); QTRY_VERIFY_WITH_TIMEOUT(Undo->isEnabled(),45000);
        QVERIFY(QFileInfo::exists(Directory.filePath("renamed_001.txt"))); QVERIFY(QFileInfo::exists(Directory.filePath("renamed_2501.txt")));
        QCOMPARE(QDir(Directory.path()).entryList({"source_*"},QDir::Files).size(),0); QCOMPARE(QDir(Directory.path()).entryList({"renamed_*"},QDir::Files).size(),2501);
        Undo->click(); QTRY_VERIFY_WITH_TIMEOUT(Dialog.findChild<QPushButton*>("hc.rename.redo")->isEnabled(),45000);
        QCOMPARE(QDir(Directory.path()).entryList({"source_*"},QDir::Files).size(),2501); QVERIFY(QDir(Directory.path()).entryList({".hc-rename-*"},QDir::Files|QDir::Hidden).isEmpty()); Dialog.CancelButton()->click();
    }
    void r9NewFileUi()
    {
        using namespace hc::ui; auto& Settings = hc::settings::AppSettings(); const auto Before = Settings.Object(); const auto Workspace = main_->CaptureWorkspace();
        const auto Restore = qScopeGuard([&] { Settings.Replace(Before); Settings.Commit(); main_->RestoreWorkspace(Workspace); main_->SaveWorkspace(); });
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        for (auto* Panel : panels_) { Panel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()),{}); QTRY_VERIFY(!model(Panel)->IsLoading()); }
        emit panels_.front()->sig_NotifyPanelActivated(); auto* Bridge = TyStCommandMgr::GetInstance()->Bridge(); QVERIFY(Bridge);
        QTRY_VERIFY(main_->findChild<QAction*>("hc.menu.file.new")->isEnabled());
        bool Opened = false;
        QTimer::singleShot(0,main_,[&] {
            auto* Dialog = static_cast<NewFileDialog*>(main_->findChild<QDialog*>("hc.newFile")); QVERIFY(Dialog);
            const auto Cleanup = qScopeGuard([&] { if (!Opened) Dialog->CancelButton()->click(); });
            Dialog->findChild<QComboBox*>("hc.newFile.type")->setCurrentIndex(6); Dialog->findChild<QLineEdit*>("hc.newFile.name")->setText("새 문서.json"); Dialog->findChild<QCheckBox*>("hc.newFile.bom")->setChecked(true);
            QVERIFY(SaveR9Dialog(*Dialog,"r9-new-file")); Opened = true; Dialog->AcceptButton()->click(); QTimer::singleShot(10000,Dialog,[Dialog] { if (Dialog->isVisible()) Dialog->CancelButton()->click(); });
        });
        main_->findChild<QAction*>("hc.menu.file.new")->trigger(); QVERIFY(Opened); QVERIFY(QFileInfo::exists(Directory.filePath("새 문서.json")));
        QFile File(Directory.filePath("새 문서.json")); QVERIFY(File.open(QIODevice::ReadOnly)); QCOMPARE(File.readAll(),QByteArray::fromHex("efbbbf")+hc::filetools::Templates()[6].text_.toUtf8()); File.close();
        QTRY_VERIFY(!model(panels_.front())->IsLoading() && names(model(panels_.front())).contains("새 문서.json"));
        const auto View = qobject_cast<CGridView*>(panels_.front()->GetFocusView(panels_.front()->CurrentTabIndex())); QVERIFY(View);
        QString Focus; for (int Row = 0; Row < model(panels_.front())->rowCount(); ++Row) { const auto Entry = model(panels_.front())->GetFileInfo(model(panels_.front())->index(Row,0)); if (Entry->Name_ == "새 문서.json") Focus = hc::workspace::SelectionKey(*Entry->storageEntry_); }
        QTRY_COMPARE(View->CaptureFocus(),Focus);
        auto Pending = Bridge->Dispatch("hc.file.new",{{"name",QString("queued.txt")},{"template",6},{"bom",true},{"queued",true}},"r9-test");
        QTRY_VERIFY(Pending.wait_for(std::chrono::milliseconds(0))==std::future_status::ready); auto Queued = Pending.get(); QVERIFY2(Queued.HasValue(),Queued.HasValue()?"":Queued.Failure().code_.c_str());
        QVERIFY(!QFileInfo::exists(Directory.filePath("queued.txt"))); QVERIFY(Bridge->FileJobs()->Scheduler().Control({Queued.Value().jobId_},hc::operations::JobControl::Start).HasValue());
        QTRY_COMPARE(Bridge->FileJobs()->Scheduler().Snapshot(Queued.Value().jobId_).Value().state_,hc::operations::JobState::Completed); QTRY_VERIFY(!Bridge->FileJobs()->IsUiPending(Queued.Value().jobId_));
        QVERIFY(QFileInfo::exists(Directory.filePath("queued.txt")));
        Pending = Bridge->Dispatch("hc.file.new",{{"name",QString("../escape")}},"r9-test"); QTRY_VERIFY(Pending.wait_for(std::chrono::milliseconds(0))==std::future_status::ready); auto Invalid = Pending.get(); QVERIFY(!Invalid.HasValue());
        Pending = Bridge->Dispatch("hc.file.new",{{"name",QString("invalid.txt")},{"template",999}},"r9-test"); QTRY_VERIFY(Pending.wait_for(std::chrono::milliseconds(0))==std::future_status::ready); Invalid = Pending.get(); QVERIFY(!Invalid.HasValue());
        Pending = Bridge->Dispatch("hc.file.new",{{"name",QString("invalid.txt")},{"template",QString("JSON")}},"r9-test"); QTRY_VERIFY(Pending.wait_for(std::chrono::milliseconds(0))==std::future_status::ready); Invalid = Pending.get(); QVERIFY(!Invalid.HasValue());
        QVERIFY(Settings.Put(hc::settings::Section::Editors,QJsonArray{QJsonObject{{"name","선택한 편집기"},{"program","C:/configured/editor.exe"},{"arguments","--open \"%FILE%\" --line 4"}}}));
        NewFileDialog Editor(Directory.path(),true,main_); Editor.show(); Editor.findChild<QLineEdit*>("hc.newFile.name")->setText("editor 한글.txt"); Editor.findChild<QCheckBox*>("hc.newFile.open")->setChecked(true); Editor.findChild<QComboBox*>("hc.newFile.editor")->setCurrentIndex(1);
        QVERIFY(SaveR9Dialog(Editor,"r9-new-file-editor")); Editor.AcceptButton()->click(); QTRY_COMPARE(Editor.result(),int(QDialog::Accepted)); QCOMPARE(Editor.EditorProgram(),QString("C:/configured/editor.exe"));
        QCOMPARE(Editor.EditorArguments(),QStringList({"--open",QDir::toNativeSeparators(Directory.filePath("editor 한글.txt")),"--line","4"}));
        for (auto* Dialog : main_->findChildren<QDialog*>()) if (Dialog->objectName()=="hc.fileJobs" || Dialog->objectName()=="hc.jobManager") Dialog->hide();
    }
    void r8CopyDefaultsUi()
    {
        using namespace hc::ui;
        auto& Settings = hc::settings::AppSettings(); const auto Before = Settings.Object();
        auto& Theme = fm::style::ThemeManager::instance(); const auto Scheme = Theme.scheme();
        const auto Restore = qScopeGuard([&] { Settings.DiscardEdit(); Settings.Replace(Before); Theme.setScheme(Scheme); });
        QTemporaryDir Directory; QVERIFY(Directory.isValid()); QVERIFY(QDir().mkdir(Directory.filePath("target")));
        if(localOnly_) for(auto* Panel:panels_) {
            if(auto* Error=Panel->findChild<QMessageBox*>("hc.navigationError")) { qInfo()<<"Isolating startup navigation failure"<<Error->text(); Error->reject(); }
            Panel->ChangeCurrentPath(QDir::toNativeSeparators(unc_),{}); QTRY_VERIFY(!model(Panel)->IsLoading());
        }
        { QFile Sample(Directory.filePath("sample.txt")); QVERIFY(Sample.open(QIODevice::WriteOnly)); QCOMPARE(Sample.write(QByteArray(262144, 'd')), qint64(262144)); }
        auto Options = Settings.Read(hc::settings::Section::Options).toObject(); Options["copyMetadata"] = true; Options["copyVerify"] = true; Options["copyAcl"] = true; Options["copyStreams"] = true; Options["copyPattern"] = "*.txt"; Options["copySpeedMiB"] = 1; QVERIFY(Settings.Put(hc::settings::Section::Options, Options));
        const auto Save = [&](QWidget* Widget, const QString& Name) {
            const auto Out = qEnvironmentVariable("HC_UI_ARTIFACT_DIR"); if (Out.isEmpty()) return; QDir().mkpath(Out); Theme.setDesign(fm::style::Design::Watercolor);
            for (const bool Dark : {false, true}) { Theme.setScheme(Dark ? fm::style::ThemeManager::Scheme::Dark : fm::style::ThemeManager::Scheme::Light); QTest::qWait(100);
                QVERIFY(Widget->grab().save(QDir(Out).filePath(Name + (Dark ? "-dark-" : "-light-") + QString::number(Widget->devicePixelRatioF()) + ".png"))); }
        };
        FileOperationDialogRequest Request; Request.command_ = "hc.file.copy"; Request.source_ = Directory.path(); Request.initialValue_ = Directory.filePath("target"); Request.names_ = {"sample.txt"}; Request.nativeDestination_ = true; Request.nativeSource_ = true;
        FileOperationDialog Existing(Request, main_); Existing.show(); QTest::qWait(80); QVERIFY(Existing.VerifyCopy() && Existing.CopyAcl() && Existing.CopyStreams()); QCOMPARE(Existing.SpeedLimit(), quint64(1024 * 1024));
        auto* Filter = Existing.findChild<QLineEdit*>("hc.operation.filter"); Filter->setText("*.txt;"); QVERIFY(!Existing.AcceptButton()->isEnabled()); QVERIFY(!Existing.findChild<QPushButton*>("hc.operation.queue")->isEnabled()); Filter->setText("*.txt"); QTRY_VERIFY(Existing.AcceptButton()->isEnabled()); Save(&Existing, "r8-copy-options");
        { QMainOpts Dialog(main_); Dialog.show(); Dialog.LoadSettings(); auto* Category = Dialog.findChild<QListWidget*>("lstOptCate"); QVERIFY(Category);
          for (int I = 0; I < Category->count(); ++I) if (Category->item(I)->text() == "파일 작업") Category->setCurrentRow(I);
          auto* Tabs = Dialog.findChild<QTabWidget*>("hc.settings.fileOperationsTabs"); QVERIFY(Tabs);
          QCOMPARE(Tabs->count(), 4); QCOMPARE(Tabs->currentIndex(), 0);
          QCOMPARE(Tabs->tabText(0), QString("공통")); QCOMPARE(Tabs->tabText(1), QString("복사·이동")); QCOMPARE(Tabs->tabText(2), QString("이름 변경")); QCOMPARE(Tabs->tabText(3), QString("삭제"));
          QVERIFY(Dialog.findChild<QComboBox*>("hc.settings.jobNotification")->isVisible());
          QVERIFY(!Dialog.findChild<QLineEdit*>("hc.settings.copyPattern")->isVisible());
          Tabs->setCurrentIndex(1);
          QCOMPARE(Dialog.findChild<QLineEdit*>("hc.settings.copyPattern")->text(), QString("*.txt"));
          bool SpeedLabelVisible = false; for (auto* Label : Dialog.findChildren<QLabel*>()) if (Label->text() == "새 복사·이동의 속도 제한" && Label->isVisible()) SpeedLabelVisible = true; QVERIFY(SpeedLabelVisible);
          Save(&Dialog, "r8-copy-settings");
          Dialog.raise(); Dialog.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&Dialog));
          Dialog.findChild<QLineEdit*>("hc.settings.copyPattern")->setText("../*"); Tabs->setCurrentIndex(3);
          QVERIFY(Dialog.findChild<QCheckBox*>("hc.settings.recycleDefault")->isVisible()); Category->setCurrentRow(0);
          QVERIFY(!Dialog.SaveSettings()); QCOMPARE(Category->currentItem()->text(), QString("파일 작업")); QCOMPARE(Tabs->currentIndex(), 1);
          QVERIFY(Dialog.findChild<QLabel*>("hc.settings.copyError")->isVisible());
          QTRY_VERIFY(Dialog.findChild<QLineEdit*>("hc.settings.copyPattern")->hasFocus());
          QCOMPARE(Settings.Read(hc::settings::Section::Options).toObject()["copyPattern"].toString(), QString("*.txt"));
          Dialog.findChild<QLineEdit*>("hc.settings.copyPattern")->setText("*.bin"); Dialog.findChild<QSpinBox*>("hc.settings.copySpeed")->setValue(2); Dialog.findChild<QCheckBox*>("hc.settings.copyVerify")->setChecked(false); QVERIFY(Dialog.SaveSettings()); Dialog.accept(); }
        QCOMPARE(Existing.IncludePattern(), QString("*.txt")); QCOMPARE(Existing.SpeedLimit(), quint64(1024 * 1024)); QVERIFY(Existing.VerifyCopy()); Existing.hide();
        { FileOperationDialog New(Request, main_); QCOMPARE(New.IncludePattern(), QString("*.bin")); QCOMPARE(New.SpeedLimit(), quint64(2 * 1024 * 1024)); QVERIFY(!New.VerifyCopy()); }
        const auto Saved = Settings.Object(); { QMainOpts Cancel(main_); Cancel.show(); Cancel.LoadSettings(); Cancel.findChild<QSpinBox*>("hc.settings.copySpeed")->setValue(9); Cancel.reject(); } QCOMPARE(Settings.Object(), Saved);
        Request.command_ = "hc.file.move"; { FileOperationDialog Move(Request, main_); Move.show(); QTest::qWait(80); QVERIFY(Move.CopyAcl()); Save(&Move, "r8-move-options"); Move.hide(); }
        Request.command_ = "hc.file.copy"; Request.nativeDestination_ = false; Request.nativeSource_ = false; Request.copyEngine_ = false;
        { FileOperationDialog Limited(Request, main_); Limited.show(); QTest::qWait(80); QVERIFY(!Limited.findChild<QCheckBox*>("hc.operation.verify")->isEnabled()); QVERIFY(!Limited.findChild<QLineEdit*>("hc.operation.filter")->isEnabled()); QCOMPARE(Limited.SpeedLimit(), quint64(0)); QVERIFY(!Limited.CopyAcl()); Save(&Limited, "r8-provider-limits"); }
        QCOMPARE(Settings.Object(), Saved);
    }
    void r8CopyCommandsUi()
    {
        using namespace hc::operations; using namespace hc::storage;
        auto& Settings = hc::settings::AppSettings(); const auto Before = Settings.Object(); const auto Workspace = main_->CaptureWorkspace(); const auto Scheme = fm::style::ThemeManager::instance().scheme();
        const auto Restore = qScopeGuard([&] { Settings.DiscardEdit(); Settings.Replace(Before); fm::style::ThemeManager::instance().setScheme(Scheme);
            if (auto* Manager = main_->findChild<QDialog*>("hc.jobManager")) Manager->hide(); if (auto* Progress = main_->findChild<QDialog*>("hc.fileJobs")) Progress->hide();
            main_->RestoreWorkspace(Workspace); });
        QTemporaryDir Directory; QVERIFY(Directory.isValid()); QVERIFY(QDir().mkdir(Directory.filePath("target"))); QVERIFY(QDir().mkdir(Directory.filePath("excluded")));
        const auto Put = [](const QString& Path, const QByteArray& Bytes) { QFile File(Path); return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size(); };
        const auto Read = [](const QString& Path) { QFile File(Path); return File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray{}; };
        QVERIFY(Put(Directory.filePath("source.txt"), QByteArray(180000, 's'))); QVERIFY(Put(Directory.filePath("source.txt:notes"), "ADS evidence"));
        auto* Panel = panels_.front(); auto* Model = static_cast<CFSModel*>(model(Panel)); Model->SetProviderEnabled(true); Panel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {}); QTRY_VERIFY(!Model->IsLoading());
        const auto IndexFor = [&] { for (int Row = 0; Row < Model->rowCount(); ++Row) if (Model->GetName(Model->index(Row, 0)) == "source.txt") return Model->index(Row, 0); return QModelIndex{}; };
        QVERIFY(IndexFor().isValid()); auto* Bridge = TyStCommandMgr::GetInstance()->Bridge(); auto* Jobs = Bridge->FileJobs(); auto& Scheduler = Jobs->Scheduler();
        auto Options = Settings.Read(hc::settings::Section::Options).toObject(); Options["copyMetadata"] = true; Options["copyVerify"] = true; Options["copyAcl"] = false; Options["copyStreams"] = true; Options["copyPattern"] = "*.txt"; Options["copySpeedMiB"] = 1; QVERIFY(Settings.Put(hc::settings::Section::Options, Options));
        std::set<std::string> OldIds; for (const auto& Job : Scheduler.Snapshots()) OldIds.insert(Job.id_);
        bool Accepted = false;
        QTimer::singleShot(100, this, [&] {
            auto* Dialog = static_cast<hc::ui::FileOperationDialog*>(main_->findChild<QDialog*>("hc.dialog.fileOperation")); QVERIFY(Dialog);
            const auto Close = qScopeGuard([&] { if (!Accepted) Dialog->reject(); });
            Dialog->findChild<QLineEdit*>("hc.operation.editor")->setText(Directory.filePath("target")+"/"); QVERIFY(Dialog->VerifyCopy()); QVERIFY(Dialog->CopyStreams());
            auto* Queue = Dialog->findChild<QPushButton*>("hc.operation.queue"); QTRY_VERIFY(Queue->isEnabled()); Accepted = true; Queue->click();
        });
        Bridge->PromptFileOperation("hc.file.copy", "test", IndexFor()); QVERIFY(Accepted);
        std::string Id; QTRY_VERIFY(([&] { for (const auto& Job : Scheduler.Snapshots()) if (!OldIds.contains(Job.id_)) { Id = Job.id_; return true; } return false; })());
        auto Snapshot = Scheduler.Snapshot(Id); QVERIFY(Snapshot.HasValue() && Snapshot.Value().deferred_); QVERIFY(!QFile::exists(Directory.filePath("target/source.txt")));
        QVERIFY(QString::fromStdString(Snapshot.Value().metadata_.options_).contains("SHA-256: 검증")); const auto CapturedOptions = Snapshot.Value().metadata_.options_;
        Options["copyVerify"] = false; Options["copyStreams"] = false; Options["copyPattern"] = "*.bin"; Options["copySpeedMiB"] = 8; QVERIFY(Settings.Put(hc::settings::Section::Options, Options));
        QCOMPARE(Scheduler.Snapshot(Id).Value().metadata_.options_, CapturedOptions); QVERIFY(Scheduler.Control({Id}, JobControl::Start).HasValue()); QTRY_COMPARE(Scheduler.Snapshot(Id).Value().state_, JobState::Completed);
        QCOMPARE(Read(Directory.filePath("target/source.txt")), QByteArray(180000, 's')); QCOMPARE(Read(Directory.filePath("target/source.txt:notes")), QByteArray("ADS evidence"));
        QTRY_VERIFY(!Jobs->IsUiPending(Id)); QTRY_VERIFY(!Model->IsLoading());
        auto Invalid = Bridge->Dispatch("hc.file.copy", {{"destination", Directory.filePath("target")}, {"copySpeed", qint64(-1)}}, "test", IndexFor()); QTRY_VERIFY(Invalid.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready); auto Rejected = Invalid.get(); QVERIFY(!Rejected.HasValue()); QCOMPARE(Rejected.Failure().code_, std::string("invalid-copy-speed"));
        auto Excluded = Bridge->Dispatch("hc.file.copy", {{"destination", Directory.filePath("excluded")}, {"copyPattern", QString("*.bin")}, {"copyVerify", true}}, "test", IndexFor()); QTRY_VERIFY(Excluded.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready); auto Submitted = Excluded.get(); QVERIFY2(Submitted.HasValue(), Submitted.HasValue() ? "" : Submitted.Failure().code_.c_str()); QTRY_COMPARE(Scheduler.Snapshot(Submitted.Value().jobId_).Value().state_, JobState::Completed); QCOMPARE(Scheduler.Snapshot(Submitted.Value().jobId_).Value().skippedItems_, uint64_t(1)); QVERIFY(!QFile::exists(Directory.filePath("excluded/source.txt")));
        QTRY_VERIFY(!Jobs->IsUiPending(Submitted.Value().jobId_)); QTRY_VERIFY(!Model->IsLoading());
        Jobs->Show(); QTest::qWait(300); const auto Out = qEnvironmentVariable("HC_UI_ARTIFACT_DIR");
        if (!Out.isEmpty()) { auto* Manager = main_->findChild<QDialog*>("hc.jobManager"); QVERIFY(Manager); auto& Theme = fm::style::ThemeManager::instance();
            for (const bool Dark : {false, true}) { Theme.setScheme(Dark ? fm::style::ThemeManager::Scheme::Dark : fm::style::ThemeManager::Scheme::Light); QTest::qWait(100); QVERIFY(Manager->grab().save(QDir(Out).filePath(QString("r8-copy-result-") + (Dark ? "dark-" : "light-") + QString::number(Manager->devicePixelRatioF()) + ".png"))); } }
    }
    void r7QueueAndRetryUi()
    {
        using namespace hc::operations; using namespace hc::storage;
        QTemporaryDir Directory; QVERIFY(Directory.isValid()); QVERIFY(QDir().mkdir(Directory.filePath("target")));
        auto Reader = MakeNativeReadProvider(LocatorKind::Local); auto Writer = MakeNativeWritableStorage(LocatorKind::Local, Reader);
        const auto Put = [](const QString& Path, const QByteArray& Bytes) { QFile File(Path); return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size(); };
        const auto Read = [](const QString& Path) { QFile File(Path); return File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray{}; };
        auto& RecoverySettings=hc::settings::AppSettings(); const auto OriginalRecoverySettings=RecoverySettings.Object();
        const auto RestoreRecovery=qScopeGuard([&]{RecoverySettings.DiscardEdit();RecoverySettings.Replace(OriginalRecoverySettings);RecoverySettings.Commit();TyStCommandMgr::GetInstance()->Bridge()->FileJobs()->ApplyDefaults();});
        auto RecoveryOptions=RecoverySettings.Read(hc::settings::Section::Options).toObject(); RecoveryOptions["enableJobRecovery"]=true;
        QVERIFY(RecoverySettings.Put(hc::settings::Section::Options,RecoveryOptions));
        QWidget Window; Window.setAttribute(Qt::WA_QuitOnClose, false); FileJobUi Jobs(&Window, std::make_shared<hc::commands::EventHub>());
        const auto Save = [&](QWidget* Widget, const QString& Name) {
            const auto Out = qEnvironmentVariable("HC_UI_ARTIFACT_DIR"); if (Out.isEmpty()) return; QDir().mkpath(Out);
            auto& Theme = fm::style::ThemeManager::instance(); Theme.setDesign(fm::style::Design::Watercolor);
            for (const bool Dark : {false, true}) { Theme.setScheme(Dark ? fm::style::ThemeManager::Scheme::Dark : fm::style::ThemeManager::Scheme::Light); QCoreApplication::processEvents();
                QVERIFY(Widget->grab().save(QDir(Out).filePath(Name + (Dark ? "-dark-" : "-light-") + QString::number(Widget->devicePixelRatioF()) + ".png"))); }
        };
        auto* Manager = Window.findChild<QDialog*>("hc.jobManager"); auto* List = Window.findChild<QTreeWidget*>("hc.jobs.list"); QVERIFY(Manager && List);
        std::vector<JobHandle> Queued;
        for (int Index = 0; Index < 3; ++Index) {
            const auto Name = QString("queued%1.txt").arg(Index); QVERIFY(Put(Directory.filePath(Name), "queued"));
            auto Entry = Reader->Resolve({LocatorKind::Local, Directory.filePath(Name).toStdString()}, {}).get(); QVERIFY(Entry.HasValue());
            FileOperationRequest Request; Request.sources_ = {{Entry.Value(), Reader, Writer}}; Request.destination_ = {LocatorKind::Local, Directory.filePath("target").toStdString()}; Request.destinationWriter_ = Writer;
            auto Handle = Jobs.Submit(Request, {}, true); QVERIFY(Handle.HasValue()); Queued.push_back(Handle.Value());
        }
        const auto Select = [&](const std::string& Id) { for (int Index = 0; Index < List->topLevelItemCount(); ++Index) if (List->topLevelItem(Index)->data(0, Qt::UserRole).toString().toStdString() == Id) List->setCurrentItem(List->topLevelItem(Index)); };
        QTRY_VERIFY(Manager->isVisible()); QTRY_VERIFY(List->topLevelItemCount() >= 3); QTest::qWait(100);
        for (int Index = 0; Index < 3; ++Index) { QVERIFY(Jobs.Scheduler().Snapshot(Queued[Index].id_).Value().deferred_); QVERIFY(!QFile::exists(Directory.filePath(QString("target/queued%1.txt").arg(Index)))); }
        Select(Queued[2].id_); Window.findChild<QPushButton*>("hc.jobs.queueUp")->click(); Window.findChild<QPushButton*>("hc.jobs.queueUp")->click();
        QVERIFY(Jobs.Scheduler().Snapshot(Queued[2].id_).Value().queueOrder_ < Jobs.Scheduler().Snapshot(Queued[0].id_).Value().queueOrder_);
        auto* Limit = Window.findChild<QSpinBox*>("hc.jobs.concurrency"); QVERIFY(Limit); Limit->setValue(1); QCOMPARE(Jobs.Scheduler().ConcurrencyLimit(), size_t(1));
        Save(Manager, "r7-queue-receipts");
        Window.findChild<QPushButton*>("hc.jobs.all.1")->click(); Window.findChild<QPushButton*>("hc.jobs.startQueue")->click();
        for (const auto& Handle : Queued) QTRY_COMPARE(Jobs.Scheduler().Snapshot(Handle.id_).Value().state_, JobState::Paused);
        QTest::qWait(300); QVERIFY(!QFile::exists(Directory.filePath("target/queued2.txt"))); Save(Manager, "r7-queue-paused");
        Window.findChild<QPushButton*>("hc.jobs.all.2")->click();
        for (const auto& Handle : Queued) { QTRY_VERIFY(Handle.completion_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready); QCOMPARE(Handle.completion_.get().snapshot_.state_, JobState::Completed); }
        FileOperationRequest Request; Request.destination_ = {LocatorKind::Local, Directory.filePath("target").toStdString()}; Request.destinationWriter_ = Writer; Request.conflict_ = ConflictPolicy::Replace;
        for (const auto& Name : {"first.txt", "failed.txt", "unattempted.txt"}) { QVERIFY(Put(Directory.filePath(Name), Name)); auto Entry = Reader->Resolve({LocatorKind::Local, Directory.filePath(Name).toStdString()}, {}).get(); QVERIFY(Entry.HasValue()); Request.sources_.push_back({Entry.Value(), Reader, Writer}); }
        QVERIFY(Put(Directory.filePath("target/failed.txt"), "locked"));
        const auto LockPath = hc::filetools::NativePath(Directory.filePath("target/failed.txt"));
        HANDLE Locked = CreateFileW(reinterpret_cast<LPCWSTR>(LockPath.utf16()), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr); QVERIFY(Locked != INVALID_HANDLE_VALUE);
        const auto Unlock = qScopeGuard([&] { if (Locked != INVALID_HANDLE_VALUE) CloseHandle(Locked); });
        auto Failed = Jobs.Submit(Request, {}); QVERIFY(Failed.HasValue()); QTRY_VERIFY(!Jobs.IsUiPending(Failed.Value().id_));
        QCOMPARE(Failed.Value().completion_.get().snapshot_.state_, JobState::Failed); QCOMPARE(Read(Directory.filePath("target/first.txt")), QByteArray("first.txt"));
        Jobs.Show(); Select(Failed.Value().id_); auto* Retry = Window.findChild<QPushButton*>("hc.jobs.managerRetry"); QVERIFY(Retry); QTRY_VERIFY(Retry->isEnabled()); Save(Manager, "r7-retry-failed");
        CloseHandle(Locked); Locked = INVALID_HANDLE_VALUE; QVERIFY(QFile::remove(Directory.filePath("target/failed.txt"))); QVERIFY(Put(Directory.filePath("target/first.txt"), "completed-item-preserved"));
        Retry->click(); QTRY_VERIFY(!Jobs.Scheduler().Snapshot(Failed.Value().id_).Value().retriedAs_.empty());
        const auto RetryId = Jobs.Scheduler().Snapshot(Failed.Value().id_).Value().retriedAs_; QTRY_VERIFY(!Jobs.IsUiPending(RetryId));
        QCOMPARE(Jobs.Scheduler().Snapshot(RetryId).Value().state_, JobState::Completed);
        QCOMPARE(Read(Directory.filePath("target/failed.txt")), QByteArray("failed.txt")); QCOMPARE(Read(Directory.filePath("target/first.txt")), QByteArray("completed-item-preserved")); QVERIFY(!QFile::exists(Directory.filePath("target/unattempted.txt")));
        Jobs.Show(); Select(Failed.Value().id_); QTRY_VERIFY(!Retry->isEnabled()); Save(Manager, "r7-retry-completed");
        QVERIFY(Put(Directory.filePath("changed.txt"), "original")); QVERIFY(Put(Directory.filePath("target/changed.txt"), "locked"));
        auto ChangedEntry = Reader->Resolve({LocatorKind::Local, Directory.filePath("changed.txt").toStdString()}, {}).get(); QVERIFY(ChangedEntry.HasValue());
        Request.sources_ = {{ChangedEntry.Value(), Reader, Writer}};
        const auto ChangedLockPath = hc::filetools::NativePath(Directory.filePath("target/changed.txt"));
        Locked = CreateFileW(reinterpret_cast<LPCWSTR>(ChangedLockPath.utf16()), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr); QVERIFY(Locked != INVALID_HANDLE_VALUE);
        auto Changed = Jobs.Submit(Request, {}); QVERIFY(Changed.HasValue()); QTRY_VERIFY(!Jobs.IsUiPending(Changed.Value().id_)); QCOMPARE(Changed.Value().completion_.get().snapshot_.state_, JobState::Failed);
        CloseHandle(Locked); Locked = INVALID_HANDLE_VALUE; QVERIFY(Put(Directory.filePath("changed.txt"), "changed-after-failure"));
        Jobs.Show(); Select(Changed.Value().id_); QTRY_VERIFY(Retry->isEnabled());
        bool Rejected = false; QTimer Dismiss; Dismiss.setInterval(10); connect(&Dismiss, &QTimer::timeout, &Window, [&] {
            for (auto* Message : Window.findChildren<QMessageBox*>()) if (Message->text().contains("retry-source-changed")) { Rejected = true; Message->accept(); }
        }); Dismiss.start(); Retry->click(); QTRY_VERIFY_WITH_TIMEOUT(Rejected, 5000); Dismiss.stop();
        QVERIFY(Jobs.Scheduler().Snapshot(Changed.Value().id_).Value().retriedAs_.empty()); QCOMPARE(Read(Directory.filePath("target/changed.txt")), QByteArray("locked"));
        QVERIFY(Put(Directory.filePath("restart-receipt.txt"), "never-auto-replay"));
        auto RestartEntry = Reader->Resolve({LocatorKind::Local, Directory.filePath("restart-receipt.txt").toStdString()}, {}).get(); QVERIFY(RestartEntry.HasValue());
        Request.sources_ = {{RestartEntry.Value(), Reader, Writer}};
        auto Receipt = Jobs.Submit(Request, {}, true); QVERIFY(Receipt.HasValue());
        const auto JournalPath = QDir(QCoreApplication::applicationDirPath()).filePath("operation-journal/" + QString::fromStdString(Receipt.Value().id_) + ".json");
        QTRY_VERIFY(QFile::exists(JournalPath));
        // A restart has no live writer handles. Reconstruct the flushed receipt
        // in a separate runtime rather than concurrently reopening the parent's
        // exclusively writable outcome files.
        // The exe's embedded qt.conf loads plugins from Libs/.
        const auto RestartRuntime = Directory.filePath("restart-runtime"); QVERIFY(QDir().mkpath(RestartRuntime + "/Libs/platforms")); QVERIFY(QDir().mkpath(RestartRuntime + "/operation-journal"));
        const auto OriginalRuntime = QCoreApplication::applicationDirPath();
        for (const auto& Name : QDir(OriginalRuntime).entryList({"*.dll"}, QDir::Files)) QVERIFY(QFile::copy(QDir(OriginalRuntime).filePath(Name), QDir(RestartRuntime).filePath(Name)));
        for (const auto& Name : QDir(OriginalRuntime + "/Libs/platforms").entryList({"qwindows*.dll"}, QDir::Files))
            QVERIFY(QFile::copy(OriginalRuntime + "/Libs/platforms/" + Name, RestartRuntime + "/Libs/platforms/" + Name));
        const auto RestartExecutable = RestartRuntime + "/hc_smb_tests.exe"; QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), RestartExecutable));
        QVERIFY(QFile::copy(JournalPath, RestartRuntime + "/operation-journal/" + QFileInfo(JournalPath).fileName()));
        QProcess Restart; auto Environment = QProcessEnvironment::systemEnvironment(); Environment.insert("HC_R7_HISTORY_ID", QString::fromStdString(Receipt.Value().id_));
        Environment.insert("HC_R7_HISTORY_TARGET", Directory.filePath("target/restart-receipt.txt")); Restart.setProcessEnvironment(Environment); Restart.setWorkingDirectory(RestartRuntime);
        // A restart reuses the user's recovery preference; a fresh config has
        // recovery disabled and intentionally does not load any previous jobs.
        Environment.insert("HC_TEST_CONFIG_DIR", QFileInfo(RecoverySettings.FileName()).absolutePath()); Restart.setProcessEnvironment(Environment);
        Restart.start(RestartExecutable, {"r7JobHistoryRestartUi", "-o", "r7-restart.txt,txt"}); QVERIFY(Restart.waitForStarted(5000));
        // Native window activation in the child must not wait on a parent UI
        // thread blocked in waitForFinished. Keep processing its window events.
        QTRY_VERIFY_WITH_TIMEOUT(Restart.state() == QProcess::NotRunning, 30000);
        if (!qEnvironmentVariable("HC_UI_ARTIFACT_DIR").isEmpty())
            QVERIFY(QFile::copy(RestartRuntime + "/r7-restart.txt", QDir(qEnvironmentVariable("HC_UI_ARTIFACT_DIR")).filePath("r7-restart-child-" + QString::fromStdString(Receipt.Value().id_) + ".txt")));
        QCOMPARE(Restart.exitCode(), 0); QVERIFY(!QFile::exists(Directory.filePath("target/restart-receipt.txt")));
        QVERIFY(Jobs.Scheduler().Cancel(Receipt.Value().id_).HasValue()); QTRY_VERIFY(!Jobs.IsUiPending(Receipt.Value().id_));
        hc::ui::FileOperationDialogRequest DialogRequest; DialogRequest.command_ = "hc.file.copy"; DialogRequest.source_ = Directory.path(); DialogRequest.initialValue_ = Directory.filePath("target"); DialogRequest.names_ = {"first.txt"}; DialogRequest.nativeDestination_ = true;
        hc::ui::FileOperationDialog Dialog(DialogRequest); Dialog.show(); QTRY_VERIFY(Dialog.AcceptButton()->isEnabled());
        auto* Queue = Dialog.findChild<QPushButton*>("hc.operation.queue"); QVERIFY(Queue && Queue->isEnabled()); Save(&Dialog, "r7-copy-queue"); Queue->click(); QVERIFY(Dialog.Queued()); QCOMPARE(Dialog.result(), int(QDialog::Accepted));
    }
    void r7JobHistoryRestartUi()
    {
        const auto Id = qEnvironmentVariable("HC_R7_HISTORY_ID"); if (Id.isEmpty()) QSKIP("Only runs as a history restart child");
        auto* Jobs = TyStCommandMgr::GetInstance()->Bridge()->FileJobs(); Jobs->Show();
        auto* Selection = main_->findChild<QComboBox*>("hc.jobs.selection"); auto* List = main_->findChild<QTreeWidget*>("hc.jobs.list"); QVERIFY(Selection && List);
        QTRY_VERIFY_WITH_TIMEOUT(Selection->findData(Id) >= 0, 10000); Selection->setCurrentIndex(Selection->findData(Id));
        QTRY_VERIFY_WITH_TIMEOUT(List->currentItem() && List->currentItem()->data(0, Qt::UserRole).toString() == Id, 5000);
        QCOMPARE(List->currentItem()->text(1), QString("중단 · 확인 필요"));
        QVERIFY(Jobs->Scheduler().Snapshots().empty()); QVERIFY(!QFile::exists(qEnvironmentVariable("HC_R7_HISTORY_TARGET")));
        auto* Details = main_->findChild<QPlainTextEdit*>("hc.jobs.details"); QVERIFY(Details);
        QTRY_VERIFY(Details->toPlainText().contains("자동 실행하지 않습니다"));
        const auto Out = qEnvironmentVariable("HC_UI_ARTIFACT_DIR");
        if (!Out.isEmpty()) {
            auto& Theme = fm::style::ThemeManager::instance(); Theme.setDesign(fm::style::Design::Watercolor);
            for (const bool Dark : {false, true}) { Theme.setScheme(Dark ? fm::style::ThemeManager::Scheme::Dark : fm::style::ThemeManager::Scheme::Light); QCoreApplication::processEvents();
                auto* Manager = main_->findChild<QDialog*>("hc.jobManager"); QVERIFY(Manager);
                QVERIFY(Manager->grab().save(QDir(Out).filePath(QString("r7-restarted-history-") + (Dark ? "dark-" : "light-") + QString::number(Manager->devicePixelRatioF()) + ".png")));
            }
        }
    }
    void todoBugTaskManagerControlsUi()
    {
        using namespace hc::operations; using namespace hc::settings;
        auto& Settings = AppSettings(); const auto Original = Settings.Object();
        const auto Restore = qScopeGuard([&] { Settings.DiscardEdit(); Settings.Replace(Original); Settings.Commit(); });
        auto Options = Settings.Read(Section::Options).toObject(); Options["enableJobRecovery"] = false; Options["jobCloseOnSuccess"] = false;
        QVERIFY(Settings.Put(Section::Options, Options));
        struct Control { std::atomic<bool> commit_{false}, finish_{false}, holdCleanup_{false}, releaseCleanup_{false}, cleaning_{false}; std::atomic<int> advances_{0}; };
        class Task final : public IJobTask {
        public:
            explicit Task(std::shared_ptr<Control> C) : control_(std::move(C)) {}
            JobStep Advance(JobStepContext) override {
                std::this_thread::sleep_for(std::chrono::milliseconds(15)); ++control_->advances_;
                JobStep Step{control_->finish_ ? JobState::Completed : control_->commit_ ? JobState::Committing : JobState::Running};
                Step.progress_ = JobProgress{uint64_t(control_->advances_) * 1024, {}, "현재 작업 항목"}; return Step;
            }
            std::vector<hc::storage::Error> Finalize(JobState) override {
                control_->cleaning_ = true;
                while (control_->holdCleanup_ && !control_->releaseCleanup_) std::this_thread::sleep_for(std::chrono::milliseconds(5));
                return {};
            }
        private: std::shared_ptr<Control> control_;
        };
        QWidget Window; Window.setAttribute(Qt::WA_QuitOnClose, false); FileJobUi Jobs(&Window, std::make_shared<hc::commands::EventHub>());
        auto Control = std::make_shared<struct Control>();
        const auto Release = qScopeGuard([&] { Control->commit_ = false; Control->releaseCleanup_ = true; });
        auto* Manager = Window.findChild<QDialog*>("hc.jobManager"); auto* List = Window.findChild<QTreeWidget*>("hc.jobs.list");
        auto* Scroll = Window.findChild<QScrollArea*>("hc.jobs.controls"); auto* Selected = Window.findChild<QLabel*>("hc.jobs.selectedSummary");
        auto* Pause = Window.findChild<QPushButton*>("hc.jobs.managerPause"); auto* Cancel = Window.findChild<QPushButton*>("hc.jobs.managerCancel");
        auto* Retry = Window.findChild<QPushButton*>("hc.jobs.managerRetry"); auto* Start = Window.findChild<QPushButton*>("hc.jobs.managerStart");
        auto* Clear = Window.findChild<QPushButton*>("hc.jobs.clearHistory"); auto* AllPause = Window.findChild<QPushButton*>("hc.jobs.all.1");
        auto* AllResume = Window.findChild<QPushButton*>("hc.jobs.all.2");
        QVERIFY(Manager && List && Scroll && Selected && Pause && Cancel && Retry && Start && Clear && AllPause && AllResume);
        Jobs.Show(); QVERIFY(!AllPause->isEnabled() && !AllResume->isEnabled() && !Clear->isEnabled());
        QVERIFY(AllPause->text().contains(QStringLiteral("일시 정지")));
        const auto Select = [&](const std::string& Id) { for (int I = 0; I < List->topLevelItemCount(); ++I) if (List->topLevelItem(I)->data(0, Qt::UserRole).toString().toStdString() == Id) List->setCurrentItem(List->topLevelItem(I)); };
        PreparedJob Work; Work.request_.metadata_ = {"현재 실행 작업", "D:/원본/<자료>&.txt", "D:/대상/선택 작업 폴더", {}}; Work.task_ = std::make_unique<Task>(Control);
        auto Running = Jobs.Submit(std::move(Work), QStringLiteral("현재 실행 작업")); QVERIFY(Running.HasValue());
        auto QueueControl = std::make_shared<struct Control>(); PreparedJob Queue; Queue.request_.deferred_ = true;
        Queue.request_.metadata_ = {"대기 작업", "D:/대기 원본", "D:/대기 대상", {}}; Queue.task_ = std::make_unique<Task>(QueueControl);
        auto Queued = Jobs.Submit(std::move(Queue), QStringLiteral("대기 작업")); QVERIFY(Queued.HasValue()); Jobs.Show();
        QTRY_VERIFY(List->topLevelItemCount() == 2); Select(Queued.Value().id_); QTRY_VERIFY(Start->isEnabled());
        QVERIFY(!Window.findChild<QPushButton*>("hc.jobs.queueUp")->isEnabled()); QVERIFY(!Window.findChild<QPushButton*>("hc.jobs.queueDown")->isEnabled());
        QTRY_VERIFY(Selected->text().contains(QStringLiteral("D:/대기 대상"))); QCOMPARE(QueueControl->advances_.load(), 0);
        QTRY_VERIFY(Window.findChild<QLabel*>("hc.jobs.batchSummary")->text().contains(QStringLiteral("D:/대상/선택 작업 폴더")));
        Select(Running.Value().id_); QTRY_VERIFY(Pause->isEnabled()); QVERIFY(!Start->isEnabled());
        QCOMPARE(Selected->textFormat(), Qt::PlainText); QVERIFY(Selected->text().contains(QStringLiteral("<자료>&.txt")));
        Control->commit_ = true; QTRY_COMPARE(Jobs.Scheduler().Snapshot(Running.Value().id_).Value().state_, JobState::Committing);
        Pause->click(); QTRY_VERIFY(Selected->text().contains(QStringLiteral("일시 정지 요청 중"))); QVERIFY(Pause->text().contains(QStringLiteral("요청 해제")));
        Control->commit_ = false; QTRY_COMPARE(Jobs.Scheduler().Snapshot(Running.Value().id_).Value().state_, JobState::Paused);
        QTRY_COMPARE(Pause->text(), QStringLiteral("재개(&P)"));
        QVERIFY(SaveR9Dialog(*Manager, "task-manager-paused")); Pause->click(); QTRY_COMPARE(Jobs.Scheduler().Snapshot(Running.Value().id_).Value().state_, JobState::Running);
        auto* Search = Window.findChild<QLineEdit*>("hc.jobs.search"); Search->setText("no-such-job-filter");
        QTRY_VERIFY(!Pause->isEnabled() && !Start->isEnabled() && !Cancel->isEnabled() && !Retry->isEnabled()); QVERIFY(Selected->text().contains(QStringLiteral("선택한 작업 없음")));
        QVERIFY(AllPause->isEnabled()); Search->clear(); Select(Running.Value().id_); QTRY_VERIFY(Cancel->isEnabled());
        QFont Large = Manager->font(); Large.setPointSize(18); Manager->setFont(Large); Manager->resize(640, 720); QTest::qWait(100);
        QCOMPARE(Manager->width(), 640); QCOMPARE(Scroll->horizontalScrollBar()->maximum(), 0);
        for (const auto* Name : {"hc.jobs.selectedGroup", "hc.jobs.queueGroup", "hc.jobs.batchGroup", "hc.jobs.historyGroup"}) {
            auto* Group = Manager->findChild<QGroupBox*>(Name); QVERIFY(Group && !Group->title().isEmpty());
            Scroll->ensureWidgetVisible(Group); QCoreApplication::processEvents(); QVERIFY(SaveR9Dialog(*Manager, QString::fromLatin1(Name)));
            for (auto* Button : Group->findChildren<QPushButton*>()) {
                Scroll->ensureWidgetVisible(Button, 0, 0); QCoreApplication::processEvents();
                QVERIFY2(Button->width() >= Button->sizeHint().width(), qPrintable(Button->text()));
                QVERIFY(Scroll->viewport()->rect().contains(QRect(Button->mapTo(Scroll->viewport(), QPoint()), Button->size())));
            }
        }
        Manager->raise(); Manager->activateWindow(); QVERIFY(QTest::qWaitForWindowActive(Manager));
        Scroll->ensureWidgetVisible(Pause); Pause->setFocus(); QTRY_VERIFY(Pause->hasFocus()); QTest::keyClick(Pause, Qt::Key_Tab); QTRY_VERIFY(Cancel->hasFocus());
        Control->commit_ = true; Control->holdCleanup_ = true; QTRY_COMPARE(Jobs.Scheduler().Snapshot(Running.Value().id_).Value().state_, JobState::Committing);
        Cancel->click(); QTRY_VERIFY(Selected->text().contains(QStringLiteral("취소 후 정리 중")));
        QVERIFY(!Cancel->isEnabled() && !Pause->isEnabled() && !Retry->isEnabled() && !Start->isEnabled());
        Control->commit_ = false; QTRY_VERIFY(Control->cleaning_.load()); QVERIFY(Jobs.IsUiPending(Running.Value().id_));
        Control->releaseCleanup_ = true; QTRY_VERIFY(!Jobs.IsUiPending(Running.Value().id_)); QTRY_VERIFY(Clear->isEnabled());
        QCOMPARE(Running.Value().completion_.get().snapshot_.state_, JobState::Cancelled); QCOMPARE(QueueControl->advances_.load(), 0);
        Clear->click(); QTRY_COMPARE(List->topLevelItemCount(), 1); QVERIFY(Jobs.Scheduler().Snapshot(Queued.Value().id_).Value().deferred_);
        QVERIFY(Jobs.Scheduler().Cancel(Queued.Value().id_).HasValue()); QTRY_VERIFY(!Jobs.IsUiPending(Queued.Value().id_));
    }
    void todoBugRenameHistoryLiveStatesUi()
    {
        using namespace hc::filetools; using namespace hc::operations;
        QTemporaryDir Directory; QVERIFY(Directory.isValid()); const auto Path = Directory.filePath("original.txt");
        { QFile File(Path); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("keep payload"); }
        QWidget Window; Window.setAttribute(Qt::WA_QuitOnClose, false); FileJobUi Jobs(&Window, std::make_shared<hc::commands::EventHub>());
        auto Prepared = PrepareBatchRename(RenameJournalDirectory(), {{Inspect(Path), Directory.filePath("renamed.txt"), {}}}); QVERIFY(Prepared.HasValue());
        const auto RecordId = QString::fromStdString(Prepared.Value().request_.metadata_.options_).mid(14); Prepared.Value().request_.deferred_ = true;
        auto Job = Jobs.SubmitRename(std::move(Prepared.Value()), QStringLiteral("이름 변경 상태 시험")); QVERIFY(Job.HasValue());
        hc::ui::RenameHistoryDialog History(&Jobs, &Window); History.show(); auto* List = History.findChild<QTreeWidget*>("hc.renameHistory.list");
        auto* Summary = History.findChild<QLabel*>("hc.renameHistory.selectedSummary"); auto* Undo = History.findChild<QPushButton*>("hc.renameHistory.undo");
        auto* Resume = History.findChild<QPushButton*>("hc.renameHistory.resume"); auto* Restore = History.findChild<QPushButton*>("hc.renameHistory.restore");
        QVERIFY(List && Summary && Undo && Resume && Restore);
        QTRY_VERIFY(([&] { for (int I = 0; I < List->topLevelItemCount(); ++I) if (List->topLevelItem(I)->data(0, Qt::UserRole).toString() == RecordId) { List->setCurrentItem(List->topLevelItem(I)); return true; } return false; })());
        QTRY_VERIFY(Summary->text().contains(QStringLiteral("대기열"))); QVERIFY(!Undo->isEnabled() && !Resume->isEnabled() && !Restore->isEnabled());
        QVERIFY(Jobs.Scheduler().Pause(Job.Value().id_, true).HasValue()); QVERIFY(Jobs.Scheduler().Control({Job.Value().id_}, JobControl::Start).HasValue());
        QTRY_COMPARE(Jobs.Scheduler().Snapshot(Job.Value().id_).Value().state_, JobState::Paused);
        QTRY_VERIFY(Summary->text().contains(QStringLiteral("일시 정지"))); QVERIFY(!Resume->isEnabled() && !Restore->isEnabled());
        QVERIFY(QFileInfo::exists(Path)); QVERIFY(SaveR9Dialog(History, "rename-history-live-paused"));
        QVERIFY(Jobs.Scheduler().Pause(Job.Value().id_, false).HasValue()); QTRY_VERIFY(!Jobs.IsUiPending(Job.Value().id_));
        QCOMPARE(Job.Value().completion_.get().snapshot_.state_, JobState::Completed);
        QTRY_VERIFY(Undo->isEnabled()); QVERIFY(!Resume->isEnabled() && !Restore->isEnabled()); QTRY_VERIFY(Summary->text().contains(QStringLiteral("완료")));
        QVERIFY(Summary->text().contains(Directory.path()) || Summary->text().contains(QDir::toNativeSeparators(Directory.path())));
        QVERIFY(QFileInfo::exists(Directory.filePath("renamed.txt"))); QVERIFY(SaveR9Dialog(History, "rename-history-live-completed"));
    }
    void todoBugFailedJobErrorUi()
    {
        using namespace hc::operations; using namespace hc::storage;
        auto& Settings = hc::settings::AppSettings(); const auto Original = Settings.Object();
        const auto Restore = qScopeGuard([&] { Settings.DiscardEdit(); Settings.Replace(Original); Settings.Commit(); });
        auto Options = Settings.Read(hc::settings::Section::Options).toObject();
        Options["showJobDetails"] = false; Options["jobCloseOnSuccess"] = true; Options["enableJobRecovery"] = false;
        QVERIFY(Settings.Put(hc::settings::Section::Options, Options));
        QWidget Window; Window.setAttribute(Qt::WA_QuitOnClose, false);
        FileJobUi Jobs(&Window, std::make_shared<hc::commands::EventHub>());
        class Task final : public IJobTask {
            JobStep Advance(JobStepContext) override {
                JobStep Step{JobState::Failed};
                Step.error_ = Error{ErrorCategory::StaleRevision, "revision-changed"};
                Step.item_ = ItemOutcome{"S01", ItemOutcome::Status::Failed, Step.error_}; return Step;
            }
            std::vector<Error> Finalize(JobState) override { return {}; }
        };
        PreparedJob Prepared; Prepared.task_ = std::make_unique<Task>();
        Prepared.request_.metadata_.source_ = R"(\\server\share\<S01>)";
        auto Started = Jobs.Submit(std::move(Prepared), QStringLiteral("삭제 오류 시험")); QVERIFY(Started.HasValue());
        QTRY_VERIFY(!Jobs.IsUiPending(Started.Value().id_));
        auto* Message = Window.findChild<QMessageBox*>("hc.jobs.failure"); QVERIFY(Message); QVERIFY(Message->isVisible());
        QCOMPARE(Message->property("hc.jobs.id").toString(), QString::fromStdString(Started.Value().id_));
        QCOMPARE(Message->textFormat(), Qt::PlainText); QCOMPARE(Message->windowModality(), Qt::NonModal);
        QVERIFY(Message->informativeText().contains("revision-changed"));
        QVERIFY(Message->informativeText().contains(QStringLiteral("다시 시도")));
        QVERIFY(Message->informativeText().contains("<S01>"));
        const auto Artifacts = qEnvironmentVariable("HC_UI_ARTIFACT_DIR");
        if (!Artifacts.isEmpty()) QVERIFY(Message->grab().save(QDir(Artifacts).filePath("smb-delete-failure.png")));
        Message->accept(); QTRY_VERIFY(Window.findChildren<QMessageBox*>("hc.jobs.failure").isEmpty());
        Jobs.Show(); QTest::qWait(250); QVERIFY(Window.findChildren<QMessageBox*>("hc.jobs.failure").isEmpty());
    }
    void r11ResponsiveJobsUi()
    {
        QWidget Window; Window.setAttribute(Qt::WA_QuitOnClose, false);
        FileJobUi Jobs(&Window, std::make_shared<hc::commands::EventHub>());
        Jobs.Show();
        auto* Manager = Window.findChild<QDialog*>("hc.jobManager"); QVERIFY(Manager);
        QFont Large = Manager->font(); Large.setPointSize(18); Manager->setFont(Large);
        Manager->resize(640, 720); QTest::qWait(100);
        const auto Out = qEnvironmentVariable("HC_UI_ARTIFACT_DIR");
        if (!Out.isEmpty()) QVERIFY(Manager->grab().save(QDir(Out).filePath("r11-manager-large-light-" + QString::number(Manager->devicePixelRatioF()) + ".png")));
        QVERIFY(SaveR9Dialog(*Manager, "r11-manager-large"));
        QCOMPARE(Manager->width(), 640);
        for (auto* Button : Manager->findChildren<QPushButton*>()) {
            if (!Button->isVisible()) continue;
            auto* Controls = Manager->findChild<QScrollArea*>("hc.jobs.controls"); QVERIFY(Controls);
            if (Controls->isAncestorOf(Button)) Controls->ensureWidgetVisible(Button, 0, 0);
            QCoreApplication::processEvents();
            QVERIFY2(Button->width() >= Button->sizeHint().width(), qPrintable(Button->text()));
            QVERIFY(Manager->rect().contains(QRect(Button->mapTo(Manager, QPoint()), Button->size())));
        }
        for (const auto& Object : {"hc.jobs.list", "hc.jobs.filter", "hc.jobs.search", "hc.jobs.concurrency"}) {
            auto* Widget = Manager->findChild<QWidget*>(Object); QVERIFY(Widget);
            auto* Accessible = QAccessible::queryAccessibleInterface(Widget); QVERIFY(Accessible);
            QVERIFY2(!Accessible->text(QAccessible::Name).isEmpty(), Object);
        }
        auto* Close = Manager->findChild<QPushButton*>("hc.jobs.close"); QVERIFY(Close);
        Close->setFocus(); QTRY_VERIFY(Close->hasFocus()); QTest::keyClick(Close, Qt::Key_Space); QTRY_VERIFY(!Manager->isVisible());
        hc::ui::FileOperationDialogRequest Request; Request.command_ = "hc.file.copy";
        Request.source_ = QStringLiteral("D:/아주 긴 한글 경로/설계 검토/원본"); Request.initialValue_ = "D:/대상 폴더";
        Request.names_ = {QStringLiteral("적용 전후 비교와 검토를 위한 매우 긴 한글 파일 이름 <자료>&.txt")};
        Request.nativeDestination_ = true; Request.providerKey_ = "local";
        hc::ui::FileOperationDialog Copy(Request); Copy.setFont(Large); Copy.resize(520, 640); Copy.show(); QTest::qWait(100);
        QVERIFY(SaveR9Dialog(Copy, "r11-copy-large"));
        auto* Scroll = Copy.findChild<QScrollArea*>(); QVERIFY(Scroll);
        QCOMPARE(Scroll->horizontalScrollBar()->maximum(), 0);
        Scroll->verticalScrollBar()->setValue(Scroll->verticalScrollBar()->maximum()); QCoreApplication::processEvents();
        QVERIFY(SaveR9Dialog(Copy, "r11-copy-options-large"));
        auto* Verify = Copy.findChild<QCheckBox*>("hc.operation.verify"); QVERIFY(Verify && Verify->isEnabled());
        Verify->setFocus(); const bool Checked = Verify->isChecked(); QTest::keyClick(Verify, Qt::Key_Space); QCOMPARE(Verify->isChecked(), !Checked);
        auto* Accessible = QAccessible::queryAccessibleInterface(Verify); QVERIFY(Accessible); QCOMPARE(Accessible->role(), QAccessible::CheckBox);
        QVERIFY(!Accessible->text(QAccessible::Name).isEmpty());
        for (auto* Button : Copy.AcceptButton()->parentWidget()->findChildren<QPushButton*>())
            QVERIFY2(Button->width() >= Button->sizeHint().width(), qPrintable(Button->text()));
        Copy.findChild<QLineEdit*>("hc.operation.editor")->setFocus(); QTest::keyClick(&Copy, Qt::Key_Escape); QVERIFY(!Copy.isVisible());
    }
    void nativeParallelSelectionUi()
    {
        using namespace hc::storage;
        using namespace hc::operations;
        const auto TargetBase = qEnvironmentVariable("HC_COPY_AUTO_TEST_TARGET");
        if (TargetBase.isEmpty()) QSKIP("Set HC_COPY_AUTO_TEST_TARGET to an owned folder on a different NVMe device");
        class ObservedReader final : public IStorageProvider {
        public:
            std::shared_ptr<IStorageProvider> inner_;
            std::mutex mutex_;
            std::set<std::thread::id> threads_;
            ProviderId Id() const override { return inner_->Id(); }
            bool SupportsParallelReads() const override { return inner_->SupportsParallelReads(); }
            ProviderCapabilities Capabilities() const override { return inner_->Capabilities(); }
            Task<SupportDecision> Supports(Operation Op, OperationContext Ctx) override { return inner_->Supports(Op, Ctx); }
            Task<std::unique_ptr<IEntryCursor>> Enumerate(Locator At, OperationContext Ctx) override { return inner_->Enumerate(At, Ctx); }
            Task<EntrySnapshot> Stat(ResourceId Id, OperationContext Ctx) override { return inner_->Stat(Id, Ctx); }
            Task<EntrySnapshot> Resolve(Locator At, OperationContext Ctx) override { return inner_->Resolve(At, Ctx); }
            Task<std::shared_ptr<IReadStream>> OpenRead(ResourceId Id, OperationContext Ctx) override {
                { std::lock_guard Lock(mutex_); threads_.insert(std::this_thread::get_id()); }
                return inner_->OpenRead(Id, Ctx);
            }
        };
        QTemporaryDir Source; QVERIFY(Source.isValid());
        QTemporaryDir Targets(QDir(TargetBase).filePath("hc-parallel-ui-XXXXXX")); QVERIFY(Targets.isValid());
        auto Reader = std::make_shared<ObservedReader>(); Reader->inner_ = MakeNativeReadProvider(LocatorKind::Local);
        auto Writer = MakeNativeWritableStorage(LocatorKind::Local, Reader);
        auto SourceRoot = Reader->Resolve({LocatorKind::Local, QDir::toNativeSeparators(Source.path()).toStdString()}, {}).get(); QVERIFY(SourceRoot.HasValue());
        auto Manifest = std::make_shared<R9Manifest>(); Manifest->provider_ = Reader;
        for (int I = 0; I < 72; ++I) {
            const auto Name = QStringLiteral("복사_%1_한글.bin").arg(I, 3, 10, QChar('0'));
            QFile File(Source.filePath(Name)); QVERIFY(File.open(QIODevice::WriteOnly));
            QCOMPARE(File.write(QByteArray(4096, char(I))), qint64(4096)); File.close();
            auto Entry = Reader->Resolve({LocatorKind::Local, QDir::toNativeSeparators(File.fileName()).toStdString()}, {}).get(); QVERIFY(Entry.HasValue());
            Manifest->entries_.push_back(Entry.Value());
        }
        auto& Settings = hc::settings::AppSettings();
        const auto Saved = Settings.CommittedObject()["options"].toObject();
        const auto Restore = qScopeGuard([&] { Settings.DiscardEdit(); Settings.BeginEdit(); Settings.Put(hc::settings::Section::Options, Saved); Settings.Commit(); });
        for (const bool Recovery : {false, true}) {
            Settings.BeginEdit(); auto Options = Saved; Options["enableJobRecovery"] = Recovery;
            Options["folderSizeBeforeOperations"] = true; Options["delayedFileWrites"] = true;
            QVERIFY(Settings.Put(hc::settings::Section::Options, Options)); QVERIFY(Settings.Commit());
            const auto Target = Targets.filePath(Recovery ? "recovery" : "parallel"); QVERIFY(QDir().mkpath(Target));
            QCOMPARE(Writer->PreferredCopyWorkers({LocatorKind::Local, Source.path().toStdString()}, {LocatorKind::Local, Target.toStdString()}), std::size_t(4));
            { std::lock_guard Lock(Reader->mutex_); Reader->threads_.clear(); }
            Manifest->maxRead_ = 0;
            QWidget Window; Window.setAttribute(Qt::WA_QuitOnClose, false);
            FileJobUi Jobs(&Window, std::make_shared<hc::commands::EventHub>());
            FileOperationRequest Request; Request.selection_ = Manifest;
            Request.selectionRoot_ = SourceRoot.Value().locator_;
            Request.destination_ = {LocatorKind::Local, QDir::toNativeSeparators(Target).toStdString()}; Request.destinationWriter_ = Writer;
            auto Started = Jobs.Submit(std::move(Request), {}); QVERIFY(Started.HasValue());
            QTRY_VERIFY_WITH_TIMEOUT(Started.Value().completion_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 30000);
            const auto Result = Started.Value().completion_.get();
            QVERIFY2(Result.snapshot_.state_ == JobState::Completed, Result.snapshot_.error_ ? Result.snapshot_.error_->code_.c_str() : "selection-copy-not-completed");
            QVERIFY(Result.outcomes_); QCOMPARE(Result.outcomes_->Count(), std::uint64_t(72));
            QCOMPARE(bool(Result.writes_), Recovery);
            if (Recovery) QCOMPARE(Result.writes_->Count(), std::uint64_t(72));
            QTRY_VERIFY(!Jobs.IsUiPending(Started.Value().id_));
            { std::lock_guard Lock(Reader->mutex_); if (Recovery) QCOMPARE(Reader->threads_.size(), std::size_t(1)); else QVERIFY(Reader->threads_.size() > 1); }
            QCOMPARE(Manifest->maxRead_.load(), Recovery ? std::size_t(1) : std::size_t(64));
            for (int I = 0; I < 72; ++I) {
                QFile File(QDir(Target).filePath(QStringLiteral("복사_%1_한글.bin").arg(I, 3, 10, QChar('0'))));
                QVERIFY(File.open(QIODevice::ReadOnly)); QCOMPARE(File.readAll(), QByteArray(4096, char(I)));
            }
        }
    }
    void backgroundJobManagerUi()
    {
        using namespace hc::operations;
        class MeasuredTask final : public IJobTask {
        public:
            explicit MeasuredTask(bool Unknown) : unknown_(Unknown) {}
            std::vector<hc::storage::Error> Finalize(JobState) override { return {}; }
            JobStep Advance(JobStepContext) override {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                JobStep Step{++count_ >= 100 ? JobState::Completed : JobState::Running};
                Step.progress_ = JobProgress{count_ * 1024, unknown_ ? std::optional<uint64_t>{} : std::optional<uint64_t>{102400}, "fixture.bin"};
                return Step;
            }
        private:
            uint64_t count_ = 0;
            bool unknown_;
        };
        QWidget Window; Window.setAttribute(Qt::WA_QuitOnClose, false);
        FileJobUi Jobs(&Window, std::make_shared<hc::commands::EventHub>());
        PreparedJob First; First.request_.resourceLanes_ = {"p3-fixture-first"}; First.task_ = std::make_unique<MeasuredTask>(false);
        auto A = Jobs.Submit(std::move(First), QStringLiteral("P3 알려진 크기 작업")); QVERIFY(A.HasValue());
        QTRY_VERIFY(Jobs.Scheduler().Snapshot(A.Value().id_).Value().progress_.bytesDone_ > 32768);
        auto* Progress = Window.findChild<QDialog*>("hc.fileJobs");
        auto* Background = Window.findChild<QPushButton*>("hc.jobs.background");
        auto* Pause = Window.findChild<QPushButton*>("hc.jobs.pause");
        auto* Toggle = Window.findChild<QPushButton*>("hc.jobs.toggleDetails");
        auto* Graph = Window.findChild<fm::ui::TransferGraph*>("hc.jobs.graph");
        QVERIFY(Progress && Background && Pause && Toggle && Graph);
        if (!Graph->isVisible()) Toggle->click();
        QTRY_VERIFY(Graph->isVisible());
        Pause->click(); QTRY_COMPARE(Jobs.Scheduler().Snapshot(A.Value().id_).Value().state_, JobState::Paused);
        QTRY_VERIFY(Graph->bytesDone() > 0);
        QTRY_VERIFY(Window.findChild<QLabel*>("hc.jobs.status")->text().startsWith(QStringLiteral("일시 정지")));
        const auto Directory = qEnvironmentVariable("HC_UI_ARTIFACT_DIR");
        const auto Save = [&](QWidget* Widget, const QString& Name) {
            if (Directory.isEmpty()) return;
            QDir().mkpath(Directory);
            auto& Theme = fm::style::ThemeManager::instance(); Theme.setDesign(fm::style::Design::Watercolor);
            for (const bool Dark : {true, false}) {
                Theme.setScheme(Dark ? fm::style::ThemeManager::Scheme::Dark : fm::style::ThemeManager::Scheme::Light);
                QCoreApplication::processEvents();
                QVERIFY(Widget->grab().save(QDir(Directory).filePath(Name + (Dark ? "-dark-" : "-light-") + QString::number(Widget->devicePixelRatioF()) + ".png")));
            }
        };
        Save(Progress, "p3-progress-paused");
        Toggle->click(); QTRY_VERIFY(!Graph->isVisible()); Save(Progress, "p3-progress-compact");
        Toggle->click(); QTRY_VERIFY(Graph->isVisible());
        Background->click(); QVERIFY(!Progress->isVisible());
        QCOMPARE(Jobs.Scheduler().Snapshot(A.Value().id_).Value().state_, JobState::Paused);
        PreparedJob Second; Second.request_.resourceLanes_ = {"p3-fixture-second"}; Second.task_ = std::make_unique<MeasuredTask>(true);
        auto B = Jobs.Submit(std::move(Second), QStringLiteral("P3 크기 미상 작업")); QVERIFY(B.HasValue());
        QTRY_VERIFY(Jobs.Scheduler().Snapshot(B.Value().id_).Value().progress_.bytesDone_ > 8192);
        Jobs.Show();
        auto* Manager = Window.findChild<QDialog*>("hc.jobManager");
        auto* List = Window.findChild<QTreeWidget*>("hc.jobs.list");
        auto* Search = Window.findChild<QLineEdit*>("hc.jobs.search");
        QVERIFY(Manager && List && Search); QTRY_VERIFY(List->topLevelItemCount() >= 2);
        Save(Manager, "p3-manager-multiple"); Save(Progress, "p3-progress-unknown");
        Search->setText("no-matching-fixture");
        for (int I = 0; I < List->topLevelItemCount(); ++I) QVERIFY(List->topLevelItem(I)->isHidden());
        Search->clear();
        auto* ManagerPause = Window.findChild<QPushButton*>("hc.jobs.managerPause");
        auto* ManagerCancel = Window.findChild<QPushButton*>("hc.jobs.managerCancel");
        QVERIFY(ManagerPause && ManagerCancel);
        for (int I = 0; I < List->topLevelItemCount(); ++I)
            if (List->topLevelItem(I)->data(0, Qt::UserRole).toString().toStdString() == A.Value().id_) List->setCurrentItem(List->topLevelItem(I));
        QTRY_VERIFY(ManagerPause->isEnabled()); ManagerPause->click();
        QTRY_VERIFY(!Jobs.Scheduler().Snapshot(A.Value().id_).Value().pauseRequested_);
        for (int I = 0; I < List->topLevelItemCount(); ++I)
            if (List->topLevelItem(I)->data(0, Qt::UserRole).toString().toStdString() == B.Value().id_) List->setCurrentItem(List->topLevelItem(I));
        QTRY_VERIFY(ManagerCancel->isEnabled()); ManagerCancel->click();
        Background->click(); Manager->hide();
        QTRY_VERIFY(A.Value().completion_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        QTRY_VERIFY(B.Value().completion_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        QCOMPARE(A.Value().completion_.get().snapshot_.state_, JobState::Completed);
        QCOMPARE(B.Value().completion_.get().snapshot_.state_, JobState::Cancelled);
        Jobs.Show(); QVERIFY(Manager->isVisible()); Save(Manager, "p3-manager-completed");
    }
    void fileJobResultPagesUi()
    {
        using namespace hc::operations;
        using namespace hc::storage;
        QTemporaryDir Directory; auto Reader = MakeNativeReadProvider(LocatorKind::Local);
        auto Writer = MakeNativeWritableStorage(LocatorKind::Local, Reader);
        FileOperationRequest Request; Request.operation_ = hc::operations::FileOperation::Delete; Request.confirmDelete_ = false;
        for (int Row = 0; Row < 201; ++Row) {
            QFile File(Directory.filePath(QString("entry%1.txt").arg(Row))); QVERIFY(File.open(QIODevice::WriteOnly)); File.close();
            auto Entry = Reader->Resolve({LocatorKind::Local, File.fileName().toStdString()}, {}).get(); QVERIFY(Entry.HasValue());
            Request.sources_.push_back({Entry.Value(), Reader, Writer});
        }
        auto Root = Reader->Resolve({LocatorKind::Local, Directory.path().toStdString()}, {}).get(); QVERIFY(Root.HasValue());
        Request.sources_ = {{Root.Value(), Reader, Writer}};
        QWidget Window; Window.setAttribute(Qt::WA_QuitOnClose, false);
        FileJobUi Jobs(&Window, std::make_shared<hc::commands::EventHub>());
        auto Job = Jobs.Submit(std::move(Request), {}); QVERIFY2(Job.HasValue(), Job.HasValue() ? "" : Job.Failure().code_.c_str());
        QTRY_VERIFY_WITH_TIMEOUT(Job.Value().completion_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 30000);
        const auto Done = Job.Value().completion_.get(); QCOMPARE(Done.snapshot_.state_, JobState::Completed);
        QCOMPARE(Done.outcomes_->Count(), 202ull); Jobs.Show();
        auto* Next = Window.findChild<QPushButton*>("hc.jobs.nextResults");
        auto* Previous = Window.findChild<QPushButton*>("hc.jobs.previousResults");
        auto* Details = Window.findChild<QPlainTextEdit*>("hc.jobs.details"); QVERIFY(Next && Previous && Details);
        QTRY_VERIFY(Next->isEnabled()); QVERIFY(!Previous->isEnabled()); Next->click();
        QTRY_VERIFY(Previous->isEnabled()); QVERIFY(!Next->isEnabled());
        auto Last = Done.outcomes_->Read(200, 1); QVERIFY(Last.HasValue());
        auto At = ParseLocator(Last.Value().items_[0].item_); QVERIFY(At.HasValue());
        QVERIFY(Details->toPlainText().contains(QString::fromStdString(At.Value().root_)));
        Previous->click(); QTRY_VERIFY(Next->isEnabled()); QVERIFY(!Previous->isEnabled());
    }
    void closingWindowCancelsWaitingFileJob()
    {
        using namespace hc::operations;
        using namespace hc::storage;
        QTemporaryDir Directory;
        QVERIFY(Directory.isValid());
        QFile File(Directory.filePath("keep.txt"));
        QVERIFY(File.open(QIODevice::WriteOnly)); File.write("keep"); File.close();
        Locator At{LocatorKind::Local, QDir::toNativeSeparators(File.fileName()).toStdString()};
        auto Reader = MakeNativeReadProvider(At.kind_);
        auto Entry = Reader->Resolve(At, {}).get(); QVERIFY(Entry.HasValue());
        QWidget Window; Window.setAttribute(Qt::WA_QuitOnClose, false); Window.show();
        FileJobUi Jobs(&Window, std::make_shared<hc::commands::EventHub>());
        FileOperationRequest Request;
        Request.operation_ = hc::operations::FileOperation::Delete;
        Request.sources_.push_back({Entry.Value(), Reader, MakeNativeWritableStorage(At.kind_, Reader)});
        auto Job = Jobs.Submit(std::move(Request), {}); QVERIFY(Job.HasValue());
        QTRY_COMPARE(Jobs.Scheduler().Snapshot(Job.Value().id_).Value().state_, JobState::WaitingForUser);
        QElapsedTimer Timer; Timer.start(); Window.close();
        QVERIFY(Timer.elapsed() < 500);
        QTRY_VERIFY(Job.Value().completion_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        QCOMPARE(Job.Value().completion_.get().snapshot_.state_, JobState::Cancelled);
        QTRY_VERIFY(!Window.isVisible());
        QVERIFY(QFile::exists(File.fileName()));
    }

    void columnMetadataTextUi()
    {
        using namespace hc::columns;
        auto* Panel = panels_.front();
        auto* View = qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex()));
        auto* Native = View->BaseView(); auto* Adapter = View->ColumnWidths();
        const auto Workspace = main_->CaptureWorkspace(); const auto Font = Native->grid()->font(); const auto Size = main_->size();
        const auto Restore = qScopeGuard([&] {
            View->setMinimumWidth(0); View->setMaximumWidth(QWIDGETSIZE_MAX); Native->grid()->setFont(Font);
            main_->resize(Size); main_->RestoreWorkspace(Workspace);
        });
        QTemporaryDir Directory; QVERIFY(Directory.isValid()); QVERIFY(QDir(Directory.path()).mkdir("folder"));
        for (const auto& Item : QList<QPair<QString, int>>{{"movie.mkv", 123456}, {"subtitle.srt", 1048575}}) {
            QFile File(Directory.filePath(Item.first)); QVERIFY(File.open(QIODevice::WriteOnly)); QVERIFY(File.resize(Item.second));
        }
        Panel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {});
        QTRY_VERIFY(!model(Panel)->IsLoading()); QTRY_COMPARE(model(Panel)->rowCount(), 4);
        auto Profile = DefaultProfile();
        Profile.id_ = QUuid::createUuid();
        for (int Index = 0; Index < Profile.columns_.size(); ++Index)
            for (auto& Width : Profile.columns_[Index].widths_) {
                Width.hidden_ = Index > 3;
                if (Index == 1) Width.value_ = 5; // Persisted narrow size ratio from the reported layout.
            }
        Profile.columns_[1].format_.size_ = SizeFormat::Adaptive;
        Profile.columns_[1].format_.precision_ = 2; Profile.columns_[1].format_.providerDisplay_ = false;
        QVERIFY(View->ApplyColumnProfile(Profile));
        const auto FullValuesFit = [&] {
            for (int Column = 1; Column <= 3; ++Column) {
                const auto* Actual = Native->getColumnByModelColumn(Column);
                int Padding = 16 + 2 * View->RecordGeometry().sideMargin;
                if (View->IsTwoLine() && Actual->visualIndex() == 1) Padding += View->TwoLineTextIndent();
                for (int Row = 0; Row < model(Panel)->rowCount(); ++Row) {
                    const auto Index = model(Panel)->index(Row, Column); const auto Text = Index.data().toString();
                    auto MetaFont = Index.data(Qt::FontRole).value<QFont>();
                    if (MetaFont.family().isEmpty()) MetaFont = Native->grid()->font();
                    MetaFont.setPixelSize(std::max(8, View->ListAppearance().fontPx - 1));
                    const QFontMetrics Metrics(MetaFont);
                    if (Metrics.elidedText(Text, Qt::ElideRight, Actual->width() - Padding) != Text) return false;
                }
            }
            return Native->horizontalScrollBar()->maximum() == 0;
        };
        for (int Pixels : {13, 24}) {
            auto Larger = Font; Larger.setPixelSize(Pixels); Native->grid()->setFont(Larger);
            for (const auto* Mode : {"one-line", "two-line"}) {
                const auto Count = Adapter->ApplyCount();
                QVERIFY(View->SetListMode(Mode)); View->setFixedWidth(1400); Adapter->Request();
                QTRY_VERIFY(Adapter->ApplyCount() > Count); QTRY_VERIFY(FullValuesFit());
                if (Pixels == 24)
                    QVERIFY(Native->grid()->grab().save(QCoreApplication::applicationDirPath() + "/column-metadata-" + Mode + ".png"));
            }
        }
    }

    void columnFitToPaneUi()
    {
        using namespace hc::columns;
        using namespace hc::view;
        auto* Panel = panels_.front();
        auto* View = qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex()));
        auto* Native = View->BaseView(); auto* Adapter = View->ColumnWidths();
        const auto Original = main_->size(); const auto Workspace = main_->CaptureWorkspace();
        const auto Font = Native->grid()->font();
        const auto Restore = qScopeGuard([&] {
            View->setMinimumWidth(0); View->setMaximumWidth(QWIDGETSIZE_MAX);
            Native->grid()->setFont(Font); main_->resize(Original); main_->RestoreWorkspace(Workspace);
        });
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        for (int Index = 0; Index < 100; ++Index) {
            QFile File(Directory.filePath(QString("long-movie-file-name-%1.mkv").arg(Index)));
            QVERIFY(File.open(QIODevice::WriteOnly));
        }
        Panel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {});
        QTRY_VERIFY(!model(Panel)->IsLoading());
        QVERIFY(View->ApplyColumnProfile(DefaultProfile())); QVERIFY(View->SetListMode("one-line"));
        QTRY_VERIFY(Native->verticalScrollBar()->isVisible());
        Native->setFocusedRowIndex(5); Native->selectRow(5);
        const auto Focus = View->CaptureFocus(); const auto Selection = View->CaptureSelection();
        QSignalSpy Reset(model(Panel), &QAbstractItemModel::modelReset);
        const auto Fits = [&] {
            QMap<int, int> Rows;
            for (int Index = 0; Index < model(Panel)->columnCount(); ++Index) {
                auto* Column = static_cast<Qtitan::GridBandedTableColumn*>(Native->getColumnByModelColumn(Index));
                if (Column->isVisible()) Rows[Column->rowIndex()] += Column->width();
            }
            for (int Width : Rows) if (Width > Adapter->AvailableWidth()) return false;
            return Native->horizontalScrollBar()->maximum() == 0;
        };
        for (int Pixels : {13, 20}) {
            auto Larger = Font; Larger.setPixelSize(Pixels); Native->grid()->setFont(Larger);
            for (const auto& Mode : {QString("one-line"), QString("two-line")}) {
                QVERIFY(View->SetListMode(Mode));
                for (int Width : {1400, 800, 500, 280, 1400}) {
                    const auto Count = Adapter->ApplyCount();
                    View->setFixedWidth(Width); Adapter->Request();
                    QTRY_VERIFY(Adapter->ApplyCount() > Count);
                    QTRY_VERIFY(Fits());
                    if (Mode == "one-line" && Width == 1400) {
                        QVERIFY(Native->getColumnByModelColumn(0)->width() > Adapter->AvailableWidth() * 0.6);
                        QVERIFY(Native->getColumnByModelColumn(2)->width() < Adapter->AvailableWidth() * 0.25);
                        QVERIFY(Native->getColumnByModelColumn(3)->width() < Adapter->AvailableWidth() * 0.1);
                    }
                }
            }
        }
        QCOMPARE(View->CaptureFocus(), Focus); QCOMPARE(View->CaptureSelection(), Selection); QCOMPARE(Reset.count(), 0);
        QVERIFY(View->SetListMode("one-line"));
        View->setFixedWidth(801);
        for (int Index = 0; Index < 4; ++Index)
            QVERIFY(Adapter->SetPolicy(Adapter->ColumnId(Index), {WidthUnit::Percent, WidthMode::Fixed, 40}, false));
        Adapter->Apply(); QTRY_VERIFY(Fits());
        for (int Index = 0; Index < 4; ++Index) QCOMPARE(Adapter->Policies()[Adapter->ColumnId(Index)].value_, 40.0);
        QVERIFY(View->ApplyColumnProfile(DefaultProfile()));
        const auto Count = Adapter->ApplyCount();
        View->setFixedWidth(1400); Adapter->Request(); QTRY_VERIFY(Adapter->ApplyCount() > Count); QTRY_VERIFY(Fits());
        QVERIFY(Native->grid()->grab().save(QCoreApplication::applicationDirPath() + "/column-fit-wide.png"));
    }

    void columnWidthLayoutAndSettings()
    {
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        QFile File(Directory.filePath("column-layout.txt")); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("columns"); File.close();
        panels_.front()->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {});
        QTRY_VERIFY_WITH_TIMEOUT(!model(panels_.front())->IsLoading(), 5000);
        QTRY_VERIFY(model(panels_.front())->rowCount() > 0);
        main_->activateWindow();
        using namespace hc::view;
        auto* View = qobject_cast<CGridView*>(panels_.front()->GetFocusView(panels_.front()->CurrentTabIndex()));
        auto* Adapter = View->ColumnWidths();
        auto* Native = View->BaseView();
        auto* Grid = Native->grid();
        QTRY_COMPARE(Adapter->Policies().size(), qsizetype(model(panels_.front())->columnCount()));
        const auto NameId = Adapter->ColumnId(0);
        const auto SizeId = Adapter->ColumnId(1);
        QVERIFY(Adapter->SetPolicy(NameId, {WidthUnit::Percent, WidthMode::Fixed, 25}));
        QVERIFY(Adapter->SetPolicy(SizeId, {WidthUnit::Percent, WidthMode::Flexible, 30, 15, 45}));
        QVERIFY(Adapter->SetPolicy(Adapter->ColumnId(2), {WidthUnit::Pixels, WidthMode::Flexible, 120}));
        QVERIFY(Adapter->SetPolicy(Adapter->ColumnId(3), {WidthUnit::Pixels, WidthMode::Flexible, 80}));
        const auto OriginalSize = main_->size();
        const auto Reset = qScopeGuard([&] { View->setMinimumWidth(0); View->setMaximumWidth(QWIDGETSIZE_MAX); main_->resize(OriginalSize); });
        QSignalSpy ModelReset(model(panels_.front()), &QAbstractItemModel::modelReset);
        // The previous fixture can leave a vertical scrollbar pending layout.
        // Measure chrome only after the two-row fixture has removed it.
        QTRY_VERIFY(!Native->verticalScrollBar()->isVisible());
        const auto FrameWidth = View->width() - Adapter->AvailableWidth();
        for (int Width : {1000, 800, 1000, 800})
        {
            View->setFixedWidth(Width + FrameWidth);
            QTRY_COMPARE(Adapter->AvailableWidth(), Width);
            QTRY_COMPARE(Native->getColumnByModelColumn(0)->width(), Width / 4);
            QCOMPARE(Native->getColumnByModelColumn(2)->width(), 120);
            QVERIFY(!Native->getColumnByModelColumn(0)->isHorSizingEnabled());
            QVERIFY(Native->getColumnByModelColumn(1)->isHorSizingEnabled());
        }
        QCOMPARE(Adapter->Policies()[NameId].value_, 25.0);
        QCOMPARE(ModelReset.count(), 0);
        Grid->viewport()->repaint();
        QRect Header, Cell;
        for (const auto& Hit : Grid->hitInfoAll())
        {
            if (Hit.columnIndex() != 0) continue;
            if (Hit.info() == Qtitan::GridHitInfo::Column) Header = Hit.rect();
            if (Hit.info() == Qtitan::GridHitInfo::Cell) Cell = Hit.rect();
        }
        QVERIFY(!Header.isEmpty()); QVERIFY(!Cell.isEmpty());
        QCOMPARE(Header.left(), Cell.left()); QCOMPARE(Header.width(), Cell.width());
        QCOMPARE(Header.width(), 200);
        QRect SizeHeader;
        for (const auto& Hit : Grid->hitInfoAll())
            if (Hit.info() == Qtitan::GridHitInfo::Column && Hit.columnIndex() == 1) SizeHeader = Hit.rect();
        QVERIFY(!SizeHeader.isEmpty());
        auto* Surface = Grid->viewport();
        const QPoint Edge(SizeHeader.right(), SizeHeader.center().y());
        const QPoint Destination = Edge + QPoint(70, 0);
        QCursor::setPos(Surface->mapToGlobal(Edge));
        QTest::mouseMove(Surface, Edge);
        QTest::mousePress(Surface, Qt::LeftButton, Qt::NoModifier, Edge);
        QCursor::setPos(Surface->mapToGlobal(Destination));
        QTest::mouseMove(Surface, Destination, 30);
        QTest::mouseRelease(Surface, Qt::LeftButton, Qt::NoModifier, Destination);
        QTRY_VERIFY(Adapter->Policies()[SizeId].value_ > 30);
        const auto Dragged = Adapter->Policies()[SizeId].value_;
        QVERIFY(Dragged <= 45);
        View->setFixedWidth(1000 + FrameWidth);
        QTRY_COMPARE(Native->getColumnByModelColumn(1)->width(), qRound(Dragged * 10));
        View->setFixedWidth(800 + FrameWidth);
        QTRY_COMPARE(Native->getColumnByModelColumn(1)->width(), qRound(Dragged * 8));
        const auto Count = Adapter->ApplyCount();
        for (int Index = 0; Index < 100; ++Index) Adapter->Request();
        QTRY_VERIFY(Adapter->ApplyCount() > Count);
        QVERIFY(Adapter->ApplyCount() - Count <= 3);
        qInfo() << "Column apply count / last nanoseconds:" << Adapter->ApplyCount() << Adapter->LastApplyNanoseconds();
        Adapter->Reload();
        QCOMPARE(Adapter->Policies()[NameId].value_, 25.0);
        auto Hidden = Adapter->Policies()[NameId]; Hidden.hidden_ = true;
        QVERIFY(Adapter->SetPolicy(NameId, Hidden));
        QTRY_VERIFY(!Native->getColumnByModelColumn(0)->isVisible());
        Hidden.hidden_ = false;
        QVERIFY(Adapter->SetPolicy(NameId, Hidden));
        QTRY_VERIFY(Native->getColumnByModelColumn(0)->isVisible());
        for (int Index = 0; Index < 4; ++Index) QVERIFY(Adapter->SetPolicy(Adapter->ColumnId(Index), {WidthUnit::Percent, WidthMode::Fixed, 40}));
        Adapter->Apply();
        QTRY_COMPARE(Native->horizontalScrollBar()->maximum(), 0);
        int FittedTotal = 0;
        for (int Index = 0; Index < 4; ++Index) {
            FittedTotal += Native->getColumnByModelColumn(Index)->width();
            QCOMPARE(Adapter->Policies()[Adapter->ColumnId(Index)].value_, 40.0);
        }
        QCOMPARE(FittedTotal, Adapter->AvailableWidth());
        QVERIFY(Adapter->SetPolicy(NameId, {WidthUnit::Percent, WidthMode::Fixed, 25}));
        QVERIFY(Adapter->SetPolicy(SizeId, {WidthUnit::Percent, WidthMode::Flexible, 30, 15, 45}));
        QVERIFY(Adapter->SetPolicy(Adapter->ColumnId(2), {WidthUnit::Pixels, WidthMode::Flexible, 120}));
        QVERIFY(Adapter->SetPolicy(Adapter->ColumnId(3), {WidthUnit::Pixels, WidthMode::Flexible, 80}));
        Native->horizontalScrollBar()->setValue(0);
        QTRY_COMPARE(Native->getColumnByModelColumn(0)->width(), 200);
        QMainOpts Dialog(main_); Dialog.SetListView(View); Dialog.ShowListSettingsPage(); Dialog.show(); QCoreApplication::processEvents();
        auto* Editor = Dialog.findChild<QtColumnProfileEditor*>(); QVERIFY(Editor);
        QVERIFY(Dialog.grab().save(QCoreApplication::applicationDirPath() + "/m3-column-settings.png"));
        Editor->findChild<QDoubleSpinBox*>("hc.columns.minimum")->setValue(30);
        QVERIFY(!Dialog.SaveSettings()); QVERIFY(!Editor->findChild<QLabel*>("hc.columns.error")->text().isEmpty()); Dialog.reject();
        QVERIFY(main_->grab().save(QCoreApplication::applicationDirPath() + "/m3-column-layout.png"));
    }

    void columnResizeVisibilityAndScrollbars()
    {
        using namespace hc::view;
        auto* Panel = panels_.front();
        auto* View = qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex()));
        auto* Adapter = View->ColumnWidths();
        auto* Native = View->BaseView();
        const auto OriginalSize = main_->size();
        QTemporaryDir Directory;
        for (int Index = 0; Index < 100; ++Index) { QFile File(Directory.filePath(QString::number(Index) + ".txt")); QVERIFY(File.open(QIODevice::WriteOnly)); }
        Panel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {});
        QTRY_VERIFY(!model(Panel)->IsLoading());
        QTRY_VERIFY(Native->verticalScrollBar()->isVisible());
        Native->setFocusedRowIndex(1);
        Native->selectRow(1);
        const auto Focused = Native->focusedRowIndex();
        QSignalSpy Reset(model(Panel), &QAbstractItemModel::modelReset);
        const auto Check = [&] { return Native->getColumnByModelColumn(0)->width() == qRound(Adapter->AvailableWidth() * 0.25); };
        main_->resize(1400, 800); QTest::qWait(30); QTRY_VERIFY(Check());
        auto* Splitter = main_->findChild<QSplitter*>("splitter");
        QVERIFY(Splitter);
        Splitter->setSizes({350, 700}); QTest::qWait(30); QTRY_VERIFY(Check());
        main_->showMaximized(); QTest::qWait(30); QTRY_VERIFY(Check());
        main_->showNormal(); QTest::qWait(30); QTRY_VERIFY(Check());
        View->hide(); QCOMPARE(Adapter->AvailableWidth(), 0);
        View->show(); QTest::qWait(30); QTRY_VERIFY(Check());
        QEvent DpiEvent(QEvent::DevicePixelRatioChange); QCoreApplication::sendEvent(View, &DpiEvent); QTRY_VERIFY(Check());
        QCOMPARE(Native->focusedRowIndex(), Focused);
        QVERIFY(Native->modelController()->isRowSelected(Native->getRow(1)));
        QCOMPARE(Reset.count(), 0);
        QCOMPARE(Adapter->Policies()[Adapter->ColumnId(0)].value_, 25.0);
        QVERIFY(main_->grab().save(QCoreApplication::applicationDirPath() + "/m3-scrollbar-layout.png"));
        Panel->ChangeCurrentPath(unc_, {}); QTRY_VERIFY(!model(Panel)->IsLoading());
        Panel->AddTab();
        auto* NewView = qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex()));
        QTRY_VERIFY(NewView->ColumnWidths()->Policies().contains(NewView->ColumnWidths()->ColumnId(0)));
        // A tab-only override does not replace the panel's new-tab default.
        QCOMPARE(NewView->ColumnWidths()->Policies()[NewView->ColumnWidths()->ColumnId(0)].value_, 46.0);
        QTRY_COMPARE(NewView->BaseView()->getColumnByModelColumn(0)->width(), NewView->ColumnWidths()->OneLineNameWidth(NewView->ColumnWidths()->AvailableWidth()));
        QTRY_COMPARE(NewView->BaseView()->horizontalScrollBar()->maximum(), 0);
        Panel->CloseTab(); QTest::qWait(30); QTRY_VERIFY(Check());
        main_->resize(OriginalSize);
    }

private:
    bool sendWlxKey(QWidget* Surface, WORD Key, bool Control)
    {
        auto* Anchor = Surface->findChild<QWidget*>("hc.wlx.native-parent"); if (!Anchor) return false;
        const auto Container = GetWindow(reinterpret_cast<HWND>(Anchor->winId()), GW_CHILD);
        const auto Child = GetWindow(Container, GW_CHILD); DWORD Process = 0;
        const auto Thread = GetWindowThreadProcessId(Child, &Process); if (!Thread || Process == GetCurrentProcessId()) return false;
        auto* Top = Surface->window(); Top->raise(); Top->activateWindow();
        const auto TopWindow = reinterpret_cast<HWND>(Top->winId());
        const auto ForegroundThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
        const auto TestThread = GetCurrentThreadId();
        // Test harness only: synchronize activation with the interactive input
        // queue, then detach before injecting keys. Product code never attaches
        // to another application's queue. No input is sent until both the
        // foreground window and native viewer focus are verified below.
        const bool Attached = ForegroundThread && ForegroundThread != TestThread && AttachThreadInput(TestThread, ForegroundThread, TRUE);
        SetForegroundWindow(TopWindow);
        if (Attached) AttachThreadInput(TestThread, ForegroundThread, FALSE);
        QTest::qWait(50);
        // Attachment can reset native focus independently of Qt's cached focus.
        // Re-enter the same focus path as a user selecting the viewer surface.
        auto* Wlx = Surface->findChild<QtPluginViewerSurface*>(); Wlx->clearFocus(); Wlx->setFocus(Qt::OtherFocusReason);
        QElapsedTimer Wait; Wait.start();
        for (;;)
        {
            GUITHREADINFO Info{sizeof(GUITHREADINFO)};
            if (GetGUIThreadInfo(Thread, &Info) && (Info.hwndFocus == Child || IsChild(Child, Info.hwndFocus))
                && GetForegroundWindow() == reinterpret_cast<HWND>(Top->winId())) break;
            if (Wait.elapsed() > 3000)
            {
                DWORD ForegroundProcess = 0; GetWindowThreadProcessId(GetForegroundWindow(), &ForegroundProcess);
                qWarning() << "WLX focus timeout" << quintptr(Child) << quintptr(Info.hwndFocus) << quintptr(GetForegroundWindow())
                    << quintptr(Top->winId()) << Thread << GetLastError() << "foreground PID" << ForegroundProcess << "test PID" << GetCurrentProcessId(); return false;
            }
            QTest::qWait(10);
        }
        std::vector<INPUT> Inputs;
        const auto Add = [&](WORD VirtualKey, bool Up) { INPUT Input{}; Input.type = INPUT_KEYBOARD; Input.ki.wVk = VirtualKey; Input.ki.dwFlags = Up ? KEYEVENTF_KEYUP : 0; Inputs.push_back(Input); };
        if (Control) Add(VK_CONTROL, false); Add(Key, false); Add(Key, true); if (Control) Add(VK_CONTROL, true);
        qInfo() << "WLX native keyboard focus verified, PID:" << Process << "key:" << Key << "control:" << Control;
        return SendInput(static_cast<UINT>(Inputs.size()), Inputs.data(), sizeof(INPUT)) == Inputs.size();
    }
    bool configureWlx(hc::plugins::PluginViewerOptions& Options, const QTemporaryDir& Directory)
    {
        using namespace hc::plugins;
        Options.binaryPath_ = Directory.filePath("viewer.wlx64");
        if (!QFile::copy(QCoreApplication::applicationDirPath() + "/hc_wlx_fixture.wlx64", Options.binaryPath_)) return false;
        QFile File(Options.binaryPath_); if (!File.open(QIODevice::ReadOnly)) return false;
        PluginManifest Manifest; Manifest.id_ = "hc.test.product-wlx"; Manifest.version_ = "1"; Manifest.binary_ = "viewer.wlx64";
        Manifest.format_ = PluginFormat::Wlx; Manifest.extensions_ = {Extension::Viewer}; Manifest.permissions_ = {Permission::ReadContent, Permission::UserInterface};
        Manifest.sha256_ = QCryptographicHash::hash(File.readAll(), QCryptographicHash::Sha256).toHex().toStdString(); File.close();
        Options.manifestPath_ = Directory.filePath("viewer.json"); Options.approval_.reviewed_ = Manifest;
        Options.approval_.approved_ = true; Options.approval_.granted_ = Manifest.permissions_;
        QFile Declaration(Options.manifestPath_); const auto Bytes = EncodeManifest(Manifest);
        return Declaration.open(QIODevice::WriteOnly) && Declaration.write(Bytes) == Bytes.size();
    }
    bool configureWcx(TyPlugInWCX& Spec, const QTemporaryDir& Directory, bool Hosted)
    {
        Spec.FileFullPath = QCoreApplication::applicationDirPath() + "/hc_wcx_fixture.wcx64";
        if (!Hosted) return true;
        using namespace hc::plugins;
        const auto Binary = Directory.filePath("fixture.wcx64");
        if (!QFile::copy(Spec.FileFullPath, Binary)) return false;
        QFile File(Binary); if (!File.open(QIODevice::ReadOnly)) return false;
        PluginManifest Manifest; Manifest.id_ = "hc.test.product-wcx"; Manifest.version_ = "1"; Manifest.binary_ = "fixture.wcx64";
        Manifest.sha256_ = QCryptographicHash::hash(File.readAll(), QCryptographicHash::Sha256).toHex().toStdString(); File.close();
        Manifest.extensions_ = {Extension::ArchiveRead, Extension::ArchiveWrite};
        Manifest.permissions_ = {Permission::ReadContent, Permission::WriteContent};
        Spec.FileFullPath = Binary; Spec.hostEnabled_ = true; Spec.hostManifest_ = Directory.filePath("plugin.json");
        Spec.hostApproval_.approved_ = true; Spec.hostApproval_.reviewed_ = Manifest; Spec.hostApproval_.granted_ = Manifest.permissions_;
        QFile Declaration(Spec.hostManifest_); const auto Bytes = EncodeManifest(Manifest);
        return Declaration.open(QIODevice::WriteOnly) && Declaration.write(Bytes) == Bytes.size();
    }
    CFsModelT* model(CmpPanel* Panel) const
    {
        auto* View = qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex()));
        return View ? View->BaseModel() : nullptr;
    }

    QStringList names(CFsModelT* Model) const
    {
        QStringList Names;
        for (int Row = 0; Row < Model->rowCount(); ++Row)
        {
            const auto Node = Model->GetFileInfo(Model->index(Row, 0));
            if (Node && Node->Name_ != "..")
                Names.push_back(Node->Name_);
        }
        Names.sort();
        return Names;
    }

    QMainUI* main_ = nullptr;
    bool localOnly_ = false;
    QList<CmpPanel*> panels_;
    QString server_;
    QString share_;
    QString unc_;
};

int main(int Argc, char** Argv)
{
    if (Argc == 2 && QByteArray(Argv[1]) == "--focus-peer") {
        QApplication App(Argc, Argv); QWidget Window; QLineEdit Editor(&Window);
        struct ActivationPeer : QAbstractNativeEventFilter {
            bool nativeEventFilter(const QByteArray&, void* Message, qintptr* Result) override {
                const auto* Native = static_cast<MSG*>(Message);
                if (Native->message != WM_APP + 117) return false;
                *Result = SetForegroundWindow(reinterpret_cast<HWND>(Native->wParam));
                return true;
            }
        } Peer;
        App.installNativeEventFilter(&Peer);
        Window.setAttribute(Qt::WA_ShowWithoutActivating); Window.show();
        std::cout << quintptr(Window.winId()) << std::endl;
        return App.exec();
    }
    if (Argc == 4 && QByteArray(Argv[1]) == "--quick-find-stream" && qEnvironmentVariableIsSet("HC_EVERYTHING_TEST_INPUT"))
        return EverythingStreamFixture(Argc, Argv);
    if (Argc >= 3 && QByteArray(Argv[1]) == "--r13-ipc-probe") {
        QCoreApplication App(Argc, Argv); hc::settings::SingleInstance Instance;
        const auto Result = Instance.Start(QString::fromLocal8Bit(Argv[2]), hc::settings::SingleInstance::Paths(App.arguments().mid(2)), [](const QJsonArray&) { return false; });
        return Result == hc::settings::SingleInstance::Result::Forwarded ? 0 : Result == hc::settings::SingleInstance::Result::Primary ? 2 : 3;
    }
    if (Argc == 2 && QByteArray(Argv[1]) == "--probe-dpr")
    {
        QApplication App(Argc, Argv);
        std::cout << App.primaryScreen()->devicePixelRatio() << std::endl;
        return 0;
    }
#ifdef _DEBUG
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    const auto ComResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::unique_ptr<DriveProbeFixture> DriveFixture;
    if (qEnvironmentVariableIsSet("HC_TEST_DISCONNECTED_DRIVE"))
    {
        DriveFixture = std::make_unique<DriveProbeFixture>();
        if (!DriveFixture->IsInstalled()) return 90;
    }
    StartupTimer.start();
    // Keep recovery history inside each copied test runtime, separate from the
    // installed application's persistent journal.
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir Config; if (!Config.isValid()) return 91;
    QList<QByteArray> Arguments; for (int Index = 0; Index < Argc; ++Index) Arguments.append(Argv[Index]);
    Arguments.append("--config-dir");
    Arguments.append(qEnvironmentVariable("HC_TEST_CONFIG_DIR", Config.path()).toUtf8());
    QList<char*> Pointers; for (auto& Argument : Arguments) Pointers.append(Argument.data()); Pointers.append(nullptr);
    int AppArgc = Arguments.size(); HyperCommanderApp App(AppArgc, Pointers.data());
    SmbTests Tests;
    const auto Result = QTest::qExec(&Tests, Argc, Argv);
    QThreadPool::globalInstance()->waitForDone();
    if (SUCCEEDED(ComResult))
        CoUninitialize();
    return Result;
}

#include "tstSmb.moc"
