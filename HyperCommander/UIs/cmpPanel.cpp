#include "stdafx.h"
#include "cmpPanel.hpp"
#include "startupTrace.hpp"
#include "UniqueLibs/qtMessageDialog.hpp"
#include "panelPathBar.hpp"
#include "UniqueLibs/quickFindSettings.hpp"
#include "UniqueLibs/qtWorkspaceRestore.hpp"
#include <fmwidgets/PanelTabStrip.h>
#include <fmwidgets/BreadcrumbBar.h>
#include <fmwidgets/DriveButton.h>
#include <fmwidgets/SegmentedControl.h>
#include <fmwidgets/PanelStatusBar.h>
#include <fmwidgets/ProgressBar.h>
#include <fmstyle/ThemeManager.h>
#include <QtConcurrent>
#include <QGridLayout>
#include "UniqueLibs/qtitanColumnAdapter.hpp"

#include "Modules/mdlFileSystem.hpp"
#include "Modules/mdlFileModel.hpp"
#include "Components/cmpView.hpp"


#include "UniqueLibs/columnMgr.hpp"
#include "UniqueLibs/commandMgr.hpp"
#include "UniqueLibs/builtInFsModel.hpp"
#include "UniqueLibs/permissionService.hpp"
#include "UniqueLibs/navigationError.hpp"
#include "UniqueLibs/externalEditorMgr.hpp"
#include "UniqueLibs/solTCPluginMgr.hpp"
#include "UniqueLibs/colorSchemeMgr.hpp"
#include "UniqueLibs/cmnHCUtils.hpp"

#include <shtypes.h>
#include <ShlObj.h>
#include <ShObjIdl.h>

#include <QtitanGrid.h>

#include "Modules/mdlFileEngine.hpp"

DECLARE_CMNLIBSV2_NAMESPACE

//////////////////////////////////////////////////////////////////////////////
///

CmpPanel::CmpPanel( QWidget* parent, Qt::WindowFlags f )
    : QWidget( parent, f )
{
    ui.setupUi( this );

    initializeUIs();

    const auto StColorSchemeMgr = TyStColorSchemeMgr::GetInstance();
    OnColorSchemeChanged( StColorSchemeMgr->GetColorScheme( StColorSchemeMgr->GetCurrentColorScheme() ) );

    QMetaObject::invokeMethod( this, "RefreshVolumeList", Qt::QueuedConnection );
    QTimer::singleShot( 100, this, [this]() {
        if( workspaceRestored_ || ui.cbxVolume->count() == 0 )
            return;

        // Start in the user's home; restored tabs keep their saved location.
        // Never probe a mapped drive just to populate the first window.
        hc::workspace::PaneState Initial;
        hc::workspace::TabState Tab; Tab.id_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
        Tab.location_ = {hc::storage::LocatorKind::Local, QDir::toNativeSeparators(QDir::homePath()).toStdString()};
        Initial.tabs_.append(Tab);
        RestoreWorkspace(Initial);
    } );
}

void CmpPanel::RefreshVolumeList() const
{
    hc::startup::Span Trace("volumes.refresh");
    const auto Previous = ui.cbxVolume->currentData().value<nsHC::TySpFileSystem>();
    const auto PreviousRoot = Previous ? Previous->GetRoot() : QString{};
    const QSignalBlocker Blocker(ui.cbxVolume);
    ui.cbxVolume->clear();
    QFileIconProvider Icons;
    const auto DriveIcon = Icons.icon(QAbstractFileIconProvider::Drive);
    const DWORD Drives = GetLogicalDrives();
    for (int Index = 0; Index < 26; ++Index) {
        if (!(Drives & (1 << Index))) continue;
        const auto Root = QString(QChar('A' + Index)) + ":\\";
        const auto Fs = nsHC::TySpFileSystem(nsHC::CFileSystemT::MakeFileSystem(Root));
        if (!Fs) continue;
        const auto Name = Fs->GetVolumeName().trimmed();
        const auto Label = QString(QChar('A' + Index)) + (Name.isEmpty() ? ":" : ":  " + Name);
        ui.cbxVolume->addItem(DriveIcon, Label, QVariant::fromValue(Fs));
    }
    const auto Network = nsHC::TySpFileSystem(nsHC::CFileSystemT::MakeFileSystem(R"(\\)"));
    if (Network) ui.cbxVolume->addItem(DriveIcon, tr("네트워크 환경"), QVariant::fromValue(Network));
    int Current = -1;
    for (int Index = 0; Index < ui.cbxVolume->count(); ++Index) {
        const auto Fs = ui.cbxVolume->itemData(Index).value<nsHC::TySpFileSystem>();
        if (Fs && Fs->GetRoot() == PreviousRoot) { Current = Index; break; }
    }
    ui.cbxVolume->setCurrentIndex(Current);
}


void CmpPanel::AddTab(bool Navigate)
{
    if (vecTabStates.size() >= 64) return;
    const int PreviousIndex = ui.tabWidget->currentIndex();
    const auto Previous = Navigate ? CaptureWorkspace(PreviousIndex) : hc::workspace::PaneState{};
    auto Columns = TyStColumnMgr::GetInstance()->GetColumnView(0);
    auto* View = static_cast<CGridView*>(createGridView());
    auto* Model = new CFSModel; auto* Proxy = new CFSProxyModel;
    View->SetModel(Model, Proxy, &Columns);
    const auto State = std::make_shared<TyTabState>(int(vecTabStates.size()), View, nullptr, Model, Proxy);
    State->workspace_.id_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    Model->setProperty("hc.tabId", State->workspace_.id_);
    vecTabStates.append(State);
    connect(View, &CViewT::sig_NotifyViewActivated, this, &CmpPanel::sig_NotifyPanelActivated);
    connect(View, &CGridView::sigViewSettingsChanged, this, &CmpPanel::sigViewSettingsChanged);
    connect(View, &CGridView::sigListPresentationChanged, this, [this, View] {
        if (retrieveFocusState() && retrieveFocusState()->View == View) syncPanelChrome();
    });
    const auto UpdateSelectionSizes = [this, State] {
        const auto Selection = static_cast<CGridView*>(State->View)->CaptureSelection();
        const QSet<QString> Keys(Selection.begin(), Selection.end());
        State->SelectedSize = 0; State->SelectedFileCount = State->SelectedDirectoryCount = 0;
        for (int Row = 0; Row < State->Model->rowCount(); ++Row) {
            const auto Item = State->Model->GetFileInfo(State->Model->index(Row, 0));
            if (!Item || !Item->storageEntry_ || !Keys.contains(hc::workspace::SelectionKey(*Item->storageEntry_))) continue;
            if (Item->storageEntry_->directory_) { ++State->SelectedDirectoryCount; State->SelectedSize += Item->directorySize_.value_or(0); }
            else { ++State->SelectedFileCount; State->SelectedSize += Item->storageEntry_->size_; }
        }
        updateWorkspaceStatus();
    };
    connect(View, &CGridView::sigThumbnailSelectionChanged, this, UpdateSelectionSizes);
    connect(Model, &CFSModel::DirectorySizesChanged, this, UpdateSelectionSizes);
    connect(Model, &QAbstractItemModel::dataChanged, this, [this, Model] {
        if (const auto Current = retrieveFocusState(); Current && Current->Model == Model) updateNavigationProgress();
    });
    connect(Model, &CFSModel::SearchResultsChanged, this, [this, Model] {
        if (retrieveFocusState() && retrieveFocusState()->Model == Model) {
            processPanelStatusText();
            if (Model->IsSearchResults()) path_->setFullPath(tr("Everything · %1").arg(Model->EverythingQuery()));
        }
    });
    connect(Model, &CFSModel::SearchRefreshRequested, this, [this, Model] {
        if (retrieveFocusState() && retrieveFocusState()->Model == Model) {
            const auto Config = hc::quickfind::Settings::FromJson(hc::settings::AppSettings().CommittedObject()["general"].toObject()["quickFind"].toObject());
            StartEverythingSearch(Model->EverythingQuery(), Config.match_, Config.everythingDisplayIntervalMs_);
        }
    });
    connect(Model, &CFSModel::sigChangedDirectory, this, &CmpPanel::OnChangedDirectory);
    connect(Model, &CFSModel::DirectoryReadStarted, this, [this, State](const QString& Path, bool Archive) {
        State->navigationStatus_ = Archive ? tr("압축 파일 여는 중…") : tr("목록 조회 중…");
        State->navigationTarget_ = Path;
        if (vecTabStates.value(ui.tabWidget->currentIndex()) == State) { updateNavigationProgress(); processPanelStatusText(); }
    });
    connect(Model, &CFSModel::MetadataChanged, Proxy, [Proxy] { Proxy->SetSearch(Proxy->Search(), Proxy->SearchMatch()); });
    connect(Model, &CFSModel::RetryRequested, this, [this, Model] {
        for (int Index = 0; Index < vecTabStates.size(); ++Index)
            if (vecTabStates[Index]->Model == Model) { restoreTab(Index); break; }
    });
    connect(Model, &CFSModel::DirectoryReadFailed, this, [this, Model](const QString& Path, bool Local) {
        const auto State = retrieveFocusState();
        const auto Error = Model->LastNavigationError();
        if (!State || State->Model != Model || !Error || !isVisible()) return;
        const auto Generation = Model->DirectoryGeneration();
        const bool Denied = Error->category_ == hc::storage::ErrorCategory::AccessDenied;
        const auto Presentation = hc::ui::DescribeNavigationError(*Error, Path);
        auto* Dialog = new hc::ui::MessageDialog(QMessageBox::Warning, tr("폴더를 열 수 없습니다"),
            tr("%1\n\n%2").arg(Presentation.summary_, QDir::toNativeSeparators(Path)),
            QMessageBox::Retry | QMessageBox::Cancel, this);
        Dialog->setObjectName("hc.navigationError");
        Dialog->setAttribute(Qt::WA_DeleteOnClose);
        Dialog->setTextFormat(Qt::PlainText);
        Dialog->setInformativeText(Presentation.recovery_);
        Dialog->setDetailedText(Presentation.details_);
        Dialog->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        Dialog->button(QMessageBox::Retry)->setText(tr("다시 시도"));
        Dialog->button(QMessageBox::Cancel)->setText(tr("취소"));
        Dialog->setDefaultButton(QMessageBox::Cancel);
        QPushButton* Elevate = nullptr;
        if (Denied && Local && !hc::permissions::Elevated()
            && hc::settings::AppSettings().Read(hc::settings::Section::Options).toObject()["allowElevation"].toBool(true)) {
            Elevate = Dialog->addButton(tr("관리자 권한으로 재시도"), QMessageBox::ActionRole);
            Elevate->setObjectName("hc.navigationElevate");
            Dialog->setInformativeText(Dialog->informativeText() + tr("\nUAC 승인 후 이 탭에서 현재 폴더와 하위 폴더의 목록을 관리자 권한으로 조회합니다. 해당 범위를 벗어나거나 탭을 닫으면 조회 권한을 종료합니다."));
        }
        // QMessageBox recalculates its fixed size on show. Keep a readable
        // content width in its layout rather than setting a transient minimum.
        if (auto* Layout = qobject_cast<QGridLayout*>(Dialog->layout()))
            Layout->addItem(new QSpacerItem(520, 0, QSizePolicy::Minimum, QSizePolicy::Minimum),
                Layout->rowCount(), 0, 1, Layout->columnCount());
        const QPointer<CFSModel> Guard(Model);
        connect(Model, &QObject::destroyed, Dialog, &QDialog::reject);
        connect(Dialog, &QDialog::finished, this, [this, Dialog, Elevate, Guard, Generation, Path](int) {
            if (!Guard || Guard->DirectoryGeneration() != Generation) return;
            const bool Retry = Dialog->clickedButton() == Dialog->button(QMessageBox::Retry);
            const bool ElevateListing = Elevate && Dialog->clickedButton() == Elevate;
            if (Retry || ElevateListing) Guard->RetryFailedNavigation(ElevateListing);
            else Guard->DismissNavigationError();
            // Restore focus after the modal window has finished hiding. Never
            // focus a different tab if navigation or tab closure superseded it.
            QTimer::singleShot(0, this, [this, Guard] {
                const auto Current = retrieveFocusState();
                if (Guard && Current && Current->Model == Guard) {
                    emit sig_NotifyPanelActivated();
                    SetFocusView(CurrentTabIndex());
                }
            });
        });
        Dialog->open();
    });
    connect(Model, &CFSModel::sigRefreshCompleted, this, [this, Model] {
        for (const auto& Tab : vecTabStates) {
            if (Tab->Model != Model) continue;
            Tab->automaticSnapshot_ = Model->IsAutomaticRefresh();
            if (Tab->restoreSelection_ && !Model->LastNavigationError()) {
                if (Tab->restoreSearchGroups_) {
                    static_cast<CGridView*>(Tab->View)->RestoreGroupExpansion(Tab->searchGroups_);
                    Tab->searchGroups_ = {}; Tab->restoreSearchGroups_ = false;
                }
                static_cast<CGridView*>(Tab->View)->RestoreSelection(Tab->workspace_.selection_, Tab->workspace_.focus_);
                if (!Tab->searchScroll_.isEmpty()) {
                    static_cast<CGridView*>(Tab->View)->RestoreScrollState(Tab->searchScroll_); Tab->searchScroll_ = {};
                }
                Tab->restoreSelection_ = false;
            }
            const auto Selection = static_cast<CGridView*>(Tab->View)->CaptureSelection();
            const QSet<QString> Keys(Selection.begin(), Selection.end());
            Tab->SelectedSize = 0; Tab->SelectedFileCount = Tab->SelectedDirectoryCount = 0;
            for (int Row = 0; Row < Model->rowCount(); ++Row) {
                const auto Item = Model->GetFileInfo(Model->index(Row, 0));
                if (!Item || !Item->storageEntry_ || !Keys.contains(hc::workspace::SelectionKey(*Item->storageEntry_))) continue;
                if (Item->storageEntry_->directory_) { ++Tab->SelectedDirectoryCount; Tab->SelectedSize += Item->directorySize_.value_or(0); }
                else { ++Tab->SelectedFileCount; Tab->SelectedSize += Item->storageEntry_->size_; }
            }
            break;
        }
        // SetPlaceholder emits completion before connecting the requested root.
        // Keep the history target until a successful directory publication;
        // failed requests leave the committed cursor unchanged and can retry.
        updateWorkspaceStatus(); syncPanelChrome();
        if (const auto Current = retrieveFocusState(); Current && Current->Model == Model)
            processVolumeStatusText(Model->GetRoot());
    });
    { QSignalBlocker Blocker(ui.tabWidget);
      ui.tabWidget->addTab(View, tr("새 탭")); ui.tabWidget->setCurrentIndex(vecTabStates.size() - 1); }
    // uic assigns the pane objectName after its constructor returns. Defer the
    // initial M3 profile lookup until that stable pane identity is available.
    if (objectName().isEmpty()) QTimer::singleShot(0, View, [View] { View->RestoreWorkspaceView({}); });
    else View->RestoreWorkspaceView({});
    const auto Color = TyStColorSchemeMgr::GetInstance();
    View->ApplyColorScheme(Color->GetColorScheme(Color->GetCurrentColorScheme()));
    if (Navigate) {
        if (!Previous.tabs_.isEmpty()) {
            auto Saved = Previous.tabs_.value(Previous.active_); Saved.id_ = State->workspace_.id_;
            Saved.view_ = View->CaptureWorkspaceView();
            State->workspace_ = Saved; State->restoreSelection_ = true;
            // A new tab inherits its panel's display defaults, while navigation
            // and selection follow the current location.
            restoreTab(vecTabStates.size() - 1);
        } else if (const auto Fs = ui.cbxVolume->currentData().value<nsHC::TySpFileSystem>()) {
            Model->SetRoot(Fs);
            Model->SetCurrentPath("\\"); Model->Refresh();
        }
    }
    if (Navigate) {
        const auto Position = hc::settings::AppSettings().Read(hc::settings::Section::General).toObject()["newTabPosition"].toString("last");
        const int Last = int(vecTabStates.size()) - 1;
        const int Target = Position == "first" ? 0 : Position == "left" ? PreviousIndex : Position == "right" ? PreviousIndex + 1 : Last;
        if (Target >= 0 && Target < Last) {
            // QTabBar::moveTab keeps the page, signals and stable tab IDs intact.
            QSignalBlocker Blocker(ui.tabWidget);
            ui.tabWidget->tabBar()->moveTab(Last, Target);
            vecTabStates.move(Last, Target);
            for (int Index = 0; Index < vecTabStates.size(); ++Index) vecTabStates[Index]->Index = Index;
            ui.tabWidget->setCurrentIndex(Target);
        }
    }
    if (Navigate) on_tabWidget_currentChanged(ui.tabWidget->currentIndex());
    View->AdjustSizeToContents();
}

void CmpPanel::NotifyDriveRemoved(const QString& Root)
{
    if (Root.isEmpty()) return;
    const auto Saved = CaptureWorkspace();
    for (int Index = 0; Index < vecTabStates.size(); ++Index) {
        const auto Location = Saved.tabs_[Index].location_;
        if (Location.kind_ != hc::storage::LocatorKind::Local && Location.kind_ != hc::storage::LocatorKind::Archive) continue;
        if (!QDir::toNativeSeparators(QString::fromStdString(Location.root_)).startsWith(QDir::toNativeSeparators(Root), Qt::CaseInsensitive)) continue;
        const auto State = vecTabStates[Index]; State->workspace_ = Saved.tabs_[Index];
        State->Model->setProperty("hc.restoreTicket", QUuid::createUuid().toString());
        State->Model->setProperty("hc.restorePending", false);
        static_cast<CFSModel*>(State->Model)->SetPlaceholder(Location, tr("드라이브가 제거되었습니다."));
    }
    ++capacityGeneration_;
    RefreshVolumeList(); updateWorkspaceStatus(); syncPanelChrome();
}

hc::workspace::PaneState CmpPanel::CaptureWorkspace(int OnlyTab) const
{
    hc::workspace::PaneState Result; Result.active_ = OnlyTab >= 0 ? 0 : std::max(0, ui.tabWidget->currentIndex());
    for (int Index = 0; Index < vecTabStates.size(); ++Index) {
        if (OnlyTab >= 0 && Index != OnlyTab) continue;
        const auto& State = vecTabStates[Index];
        auto Tab = State->workspace_;
        const auto Model = static_cast<CFSModel*>(State->Model);
        const auto View = static_cast<CGridView*>(State->View);
        if (State->searchOrigin_) {
            Tab = *State->searchOrigin_;
            Tab.view_["listPresentation"] = View->CaptureWorkspaceView()["listPresentation"];
            Result.tabs_.append(std::move(Tab)); continue;
        }
        if (!State->pending_) {
            Tab.location_ = Model->WorkspaceLocation();
            if (!Model->GetRoot() && !Model->HasPlaceholder() && Tab.location_.root_.empty())
                Tab.location_ = {hc::storage::LocatorKind::Local, QDir::toNativeSeparators(QDir::homePath()).toStdString()};
            if (Tab.location_.kind_ == hc::storage::LocatorKind::Plugin || Tab.location_.kind_ == hc::storage::LocatorKind::Archive) {
                Tab.provider_ = Model->property("hc.workspaceProvider").toString();
                Tab.nativeRoot_ = Model->property("hc.workspaceNativeRoot").toString();
                if (const auto Pack = dynamic_cast<nsHC::CFSPack*>(Model->GetRoot().get()); Pack && Pack->GetWCX())
                    for (const auto& Choice : TyStPlugInMgr::GetInstance()->RetrieveWCXByExts(QFileInfo(Pack->GetRoot()).suffix(), Pack->GetRoot()))
                        if (Choice.FileFullPath.compare(Pack->GetWCX()->PluginPath(), Qt::CaseInsensitive) == 0) { Tab.provider_ = Choice.hostManifest_; break; }
            } else { Tab.provider_.clear(); Tab.nativeRoot_.clear(); }
            if (!State->restoreSelection_) { Tab.selection_ = View->CaptureSelection(); Tab.focus_ = View->CaptureFocus(); }
        }
        if (Tab.location_.kind_ == hc::storage::LocatorKind::Plugin) {
            const auto Path = hc::workspace::PluginPath(Tab.location_);
            if (!Path.isEmpty()) Tab.location_.root_ = Path.toStdString();
        }
        Tab.view_ = View->CaptureWorkspaceView(); Result.tabs_.append(std::move(Tab));
    }
    return Result;
}

void CmpPanel::RestoreWorkspace(const hc::workspace::PaneState& Pane, const QJsonObject& Defaults)
{
    if (Pane.tabs_.isEmpty() || Pane.tabs_.size() > 64 || Pane.active_ < 0 || Pane.active_ >= Pane.tabs_.size()) return;
    workspaceRestored_ = true;
    // A preview temporarily replaces its grid in QTabWidget. Restore that grid
    // first so replacing a workspace also destroys its hidden model/watchers.
    for (int Index = 0; Index < vecTabStates.size(); ++Index)
        if (vecTabStates[Index]->QuickView) CloseQuickView(Index);
    { QSignalBlocker Blocker(ui.tabWidget);
      // Removing a focused tab synchronously emits application focus events.
      // Retire all old state pointers before deleting any grid: the main window
      // updates both panels' appearance from that event filter.
      vecTabStates.clear();
      while (ui.tabWidget->count()) { auto* Widget = ui.tabWidget->widget(0); ui.tabWidget->removeTab(0); delete Widget; } }
    for (const auto& Tab : Pane.tabs_) {
        AddTab(false); const auto State = vecTabStates.back();
        State->workspace_ = Tab; State->pending_ = true; State->restoreSelection_ = true;
        State->Model->setProperty("hc.tabId", Tab.id_);
        static_cast<CGridView*>(State->View)->RestoreWorkspaceView(hc::workspace::MergeView(Defaults, Tab.view_));
        ui.tabWidget->setTabText(vecTabStates.size() - 1, QString::fromStdString(Tab.location_.root_));
    }
    { QSignalBlocker Blocker(ui.tabWidget); ui.tabWidget->setCurrentIndex(Pane.active_); }
    on_tabWidget_currentChanged(Pane.active_);
}

void CmpPanel::restoreTab(int Index)
{
    if (Index < 0 || Index >= vecTabStates.size()) return;
    const auto State = vecTabStates[Index]; State->pending_ = false;
    static_cast<CFSModel*>(State->Model)->StopEverythingSearch(true); State->searchOrigin_.reset();
    static_cast<CGridView*>(State->View)->SetEverythingPresentation(false);
    // Reuse discovered volumes, retaining their cached labels and capabilities
    // without another filesystem-information query during workspace restore.
    nsHC::TySpFileSystem Native;
    const auto Path = QDir::toNativeSeparators(QString::fromStdString(State->workspace_.location_.root_));
    for (int Volume = 0; Volume < ui.cbxVolume->count(); ++Volume) {
        const auto Fs = ui.cbxVolume->itemData(Volume).value<nsHC::TySpFileSystem>();
        if (Fs && !Fs->GetRoot().isEmpty() && Path.startsWith(Fs->GetRoot(), Qt::CaseInsensitive)) { Native = Fs; break; }
    }
    RestoreWorkspaceLocation(static_cast<CFSModel*>(State->Model), State->workspace_, Native);
}

void CmpPanel::updateWorkspaceStatus()
{
    updateNavigationProgress();
    if (!workspaceStatus_ || ui.tabWidget->currentIndex() < 0 || ui.tabWidget->currentIndex() >= vecTabStates.size()) return;
    const auto Model = static_cast<CFSModel*>(vecTabStates[ui.tabWidget->currentIndex()]->Model);
    const auto Location = Model->WorkspaceLocation();
    int Volume = -1;
    if (Location.kind_ == hc::storage::LocatorKind::Local || Location.kind_ == hc::storage::LocatorKind::Smb || Location.kind_ == hc::storage::LocatorKind::Archive) {
        const auto Path = QDir::toNativeSeparators(QString::fromStdString(Location.root_));
        for (int Index = 0; Index < ui.cbxVolume->count(); ++Index) {
            const auto Fs = ui.cbxVolume->itemData(Index).value<nsHC::TySpFileSystem>();
            if (Fs && !Fs->GetRoot().isEmpty() && Path.startsWith(Fs->GetRoot(), Qt::CaseInsensitive)) { Volume = Index; break; }
        }
    }
    if (ui.cbxVolume->currentIndex() != Volume) {
        QSignalBlocker Blocker(ui.cbxVolume); ui.cbxVolume->setCurrentIndex(Volume);
        ui.lblVolumeStatus->setText(Model->GetRoot() ? Model->GetRoot()->GetRoot() : QString{});
    }
    const auto Error = Model->LastNavigationError();
    const auto ErrorText = Error ? hc::ui::DescribeNavigationError(*Error, QString::fromStdString(Model->WorkspaceLocation().root_)) : hc::ui::NavigationErrorText{};
    workspaceStatus_->setText(Error ? tr("%1 · %2").arg(QDir::toNativeSeparators(QString::fromStdString(Model->WorkspaceLocation().root_)), ErrorText.summary_) : QString{});
    workspaceStatus_->setToolTip(Error ? ErrorText.recovery_ + "\n\n" + ErrorText.details_ : QString{});
    workspaceStatus_->setVisible(bool(Error)); retry_->setVisible(bool(Error));
    workspaceStatus_->parentWidget()->setVisible(bool(Error));
    retry_->setEnabled(!Model->property("hc.restorePending").toBool());
    processPanelStatusText();
}


void CmpPanel::PrevTab()
{
    // 탭이 하나라면 아무 것도 하지 않는다. 
    if( ui.tabWidget->count() <= 1 )
        return;

    // TODO: 볼륨 목록 동기화

    const auto Current = ui.tabWidget->currentIndex();
    Q_ASSERT( Current >= 0 );
    if( Current == 0 )
        ui.tabWidget->setCurrentIndex( ui.tabWidget->count() - 1 );
    else
        ui.tabWidget->setCurrentIndex( Current - 1 );
}

void CmpPanel::NextTab()
{
    // 탭이 하나라면 아무 것도 하지 않는다. 
    if( ui.tabWidget->count() <= 1 )
        return;

    // TODO: 볼륨 목록 동기화

    const auto Current = ui.tabWidget->currentIndex();
    Q_ASSERT( Current >= 0 );
    if( Current == ui.tabWidget->count() - 1 )
        ui.tabWidget->setCurrentIndex( 0 );
    else
        ui.tabWidget->setCurrentIndex( Current + 1 );
}

void CmpPanel::CloseTab() { closeTabAt(ui.tabWidget->currentIndex()); }

void CmpPanel::closeTabAt(int Current)
{
    // 마지막 탭이라면 닫지 않는다.
    if( ui.tabWidget->count() == 1 )
        return;

    if (Current < 0 || Current >= vecTabStates.size()) return;
    const auto ActiveId = vecTabStates[ui.tabWidget->currentIndex()]->workspace_.id_;
    { QSignalBlocker Blocker(ui.tabWidget); ui.tabWidget->setCurrentIndex(Current); }
    // Restore the original grid before deleting a preview tab so neither its
    // hidden model nor an active viewer session survives tab closure.
    if (vecTabStates[Current]->QuickView) CloseQuickView(Current);
    const auto Closing = ui.tabWidget->widget(Current);
    {
        QSignalBlocker Blocker(ui.tabWidget);
        ui.tabWidget->removeTab(Current);
        vecTabStates.removeAt(Current);
        int Active = std::min(Current, ui.tabWidget->count() - 1);
        for (int Index = 0; Index < vecTabStates.size(); ++Index) if (vecTabStates[Index]->workspace_.id_ == ActiveId) Active = Index;
        ui.tabWidget->setCurrentIndex(Active);
    }
    delete Closing;
    on_tabWidget_currentChanged(ui.tabWidget->currentIndex());
    SetFocusView(ui.tabWidget->currentIndex());
}

int CmpPanel::CurrentTabIndex() const
{
    return retrieveCurrentIndex();
}

void CmpPanel::SetFocusView( int TabIndex )
{
    auto View = retrieveFocusView();

    if( View == nullptr )
        return;

    if( View->GetViewMode() == CViewT::VM_GRID )
        View->EnsureKeyboardFocusOnView();
}

CViewT* CmpPanel::GetFocusView( int TabIndex ) const
{
    if( TabIndex < 0 || TabIndex >= vecTabStates.size() )
        return {};

    const auto& State = vecTabStates.value( TabIndex );
    if( State->QuickView != nullptr )
        return State->QuickView;

    return State->View;
}

void CmpPanel::OpenQuickView( int TabIndex, const QString& FilePath )
{
    if( TabIndex < 0 || TabIndex >= vecTabStates.size() )
        return;

    if (vecTabStates[TabIndex]->QuickView) CloseQuickView(TabIndex);

    {
        QSignalBlocker Blocker( ui.tabWidget );
        const auto& State = vecTabStates.value( TabIndex );
        const auto& TabTitle = ui.tabWidget->tabText( TabIndex );

        ui.tabWidget->removeTab( TabIndex );
        State->QuickView = new CQuickView;
        ( ( CQuickView* )State->QuickView )->SetFilePath( FilePath );
        ui.tabWidget->insertTab( TabIndex, State->QuickView, TabTitle );
        ui.tabWidget->setCurrentIndex( TabIndex );
    }
}

void CmpPanel::CloseQuickView( int TabIndex )
{
    Q_ASSERT( TabIndex >= 0 && TabIndex < ui.tabWidget->count() );
    if( TabIndex < 0 || TabIndex >= ui.tabWidget->count() )
        return;

    {
        QSignalBlocker Blocker( ui.tabWidget );
        const auto& State = vecTabStates[ TabIndex ];
        const auto& TabTitle = ui.tabWidget->tabText( TabIndex );

        if( State->QuickView == nullptr )
            return;

        State->QuickView->CloseView();
        ui.tabWidget->removeTab( TabIndex );
        State->QuickView->deleteLater();
        State->QuickView = nullptr;
        ui.tabWidget->insertTab( TabIndex, State->View, TabTitle );
        ui.tabWidget->setCurrentIndex( TabIndex );
    }
}

void CmpPanel::RefreshSource( int TabIndex )
{
    if( TabIndex < 0 || TabIndex >= vecTabStates.size() )
        return;

    if (static_cast<CFSModel*>(vecTabStates[TabIndex]->Model)->HasPlaceholder()) restoreTab(TabIndex);
    else if (!static_cast<CFSModel*>(vecTabStates[TabIndex]->Model)->RetryFailedNavigation()) vecTabStates[TabIndex]->Model->Refresh();
}

void CmpPanel::SelectRowOnCurrentTab( const QModelIndex& SrcIndex, bool IsMoveDown )
{
    const auto State = retrieveFocusState();
    const auto View = retrieveFocusView();
    Q_ASSERT( View != nullptr );
    if( View == nullptr )
        return;

    if( View->GetViewMode() != CViewT::VM_GRID )
        return;

    bool IsSelected = false;

    const auto& Info = ( ( CGridView* )View )->SelectRowOnCurrentTab( SrcIndex, IsMoveDown, IsSelected );

    if( Info == nullptr )
        return;

    if( Info->Name_ == ".." )
        return;

    if( IsSelected == true )
    {
        if( FlagOn( Info->Attributes_, FILE_ATTRIBUTE_DIRECTORY ) )
            State->SelectedDirectoryCount += 1;
        else
            State->SelectedFileCount += 1;

        State->SelectedSize += (Info->Attributes_ & FILE_ATTRIBUTE_DIRECTORY) ? Info->directorySize_.value_or(0) : Info->Size_;
    }
    else
    {
        if( FlagOn( Info->Attributes_, FILE_ATTRIBUTE_DIRECTORY ) )
            State->SelectedDirectoryCount -= 1;
        else
            State->SelectedFileCount -= 1;

        State->SelectedSize -= (Info->Attributes_ & FILE_ATTRIBUTE_DIRECTORY) ? Info->directorySize_.value_or(0) : Info->Size_;
    }

    QMetaObject::invokeMethod( this, "processPanelStatusText", Qt::QueuedConnection );
}

void CmpPanel::ReturnOnCurrentTab( const QModelIndex& SrcIndex )
{
    auto State = retrieveFocusState();
    if( State == nullptr )
        return;

    const auto ModelIndex = State->ConvertToSrcIndex( SrcIndex );
    const auto EntryInfo = State->Model->GetFileInfo( ModelIndex );
    if (!EntryInfo) return;

    if( EntryInfo->Attributes_ & FILE_ATTRIBUTE_DIRECTORY )
    {
        if (static_cast<CFSModel*>(State->Model)->IsSearchResults()) {
            ChangeCurrentPath(State->Model->GetFileFullPath(ModelIndex), {}); return;
        }
        State->Model->ChangeDirectory( ModelIndex );
    }
    else
    {
        // 해당 확장자를 처리할 수 있다고 주장하는 플러그인이 있다면 처리를 맡긴다. 
        const auto StPlugInMgr = TyStPlugInMgr::GetInstance();
        std::wstring Src = State->Model->GetFileFullPath( ModelIndex ).toStdWString();
        const auto VecWCXs = StPlugInMgr->RetrieveWCXByExts( EntryInfo->Ext_, EntryInfo->Name_ );

        if( VecWCXs.count() > 0 )
        {
            if (static_cast<CFSModel*>(State->Model)->IsSearchResults()) StopEverythingSearch(false);
            State->Model->ChangeDirectory( ModelIndex );
            return;
        }

        // TODO: 해당 항목이 내부 진입이 가능한 파일인지 확인한 후 아닐 때 실행한다.
        // NOTE: 토탈 커맨더의 경우 .DLL 등 명시적인 실행파일이 아닌 경우 레지스트리를 뒤져 실행가능하지 않다면 ShellExecuteExW 를 호출하지 않고, 오류창을 표시한다. 
        CoInitializeEx( NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE );

        do
        {
            SHELLEXECUTEINFOW shinfo = { 0, };
            shinfo.cbSize = sizeof( shinfo );
            shinfo.nShow = SW_NORMAL;
            shinfo.lpVerb = L"open";
            shinfo.lpFile = Src.c_str();

            ShellExecuteExW( &shinfo );

        } while( false );

        CoUninitialize();
    }
}

void CmpPanel::ContextMenuOnCurrentTab( const QModelIndex& SrcIndex )
{
    const auto State = retrieveFocusState();
    const auto View = retrieveFocusView();
    Q_ASSERT( View != nullptr );
    if( View == nullptr )
        return;

    if( View->GetViewMode() != CViewT::VM_GRID )
        return;

    ( ( CGridView* )View )->ContextMenuOnCurrentTab( SrcIndex );
}

void CmpPanel::ExternalEditorMenu( const QModelIndex& SrcIndex )
{
    /*!
        View 가 QuickView 인지 아닌지 확인한다. 
        해당 모델의 최상위 Fs 를 획득한다.
        Fs 가 직접적으로 파일을 획득할 수 있는지 확인한다. ( PACKER 여부 )
            PACKER 라면 임시 디렉토리에 파일을 해제할 수 있는지 확인한다. 
        선택한 것이 있는가? 없다면 현재 커서가 위치한 항목 사용
        선택항목이 하나 인가? 하나라면 해당 항목 사용
        선택항목이 여러 개라면 설정에 따라 목록을 전송하거나 첫째 항목 전송
     */

    const auto StExternalMgr = TyStExternalEditorMgr::GetInstance();
    const auto ColorScheme = TyStColorSchemeMgr::GetInstance()->GetColorScheme( TyStColorSchemeMgr::GetInstance()->GetCurrentColorScheme() );
    const auto State = retrieveFocusState();
    const auto View = State->View;
    const auto Fs = State->Model->GetRoot();

    if( View->GetViewMode() != CViewT::VM_GRID )
        return;

    const auto& VecSrcModel = makeSrcModel( State );

    if( VecSrcModel.isEmpty() == true )
    {
        // 선택된 항목이 없음
        return;
    }

    if( FlagOn( Fs->GetFeatures(), nsHC::FS_FEA_PACK ) )
    {
        // TODO: 선택된 항목들의 압축을 해제한 후, 임시 파일에 대한 목록을 가져온다. 

        Q_ASSERT( false );
    }

    QMenu Menu;
    Menu.setFocus( Qt::ActiveWindowFocusReason );
    Menu.setFocusPolicy( Qt::StrongFocus );
    Menu.setFont( QFont( ColorScheme.Dialog_Font.family(), 14 ) );

    const auto FileFullPath = State->Model->GetFileFullPath( VecSrcModel[0] );
    StExternalMgr->ConstructExternalMenu( &Menu, FileFullPath );

    // TODO: 글로벌 위치를 획득한 후, 해당 위치가 항목(SrcIndex) 영역 내에 위치하는 지 확인한다. 
    // 영역내에 위치한다면 해당 위치에 표시하고, 커서가 전혀 다른 곳에 위치한다면, 항목의 영역을 구하고 해당 위치내에서 표시한다. 
    POINT Pos = {};
    GetCursorPos( &Pos );

    qDebug() << QCursor::pos() << QPoint( Pos.x, Pos.y );

    QMetaObject::invokeMethod( &Menu, "setFocus", Qt::QueuedConnection );
    auto ArrowDownEvent = new QKeyEvent( QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier );
    qApp->postEvent( &Menu, ArrowDownEvent );   // 이벤트의 소유권은 Qt 에게로 이전되며, 자동으로 삭제된다. 
    const auto ac = Menu.exec( retrieveMenuPoint( QCursor::pos(), SrcIndex ) );

    // 취소하거나 등등
    if( ac == nullptr )
        return;
    
    const auto Editor = ac->data().value< TyExternalEditor >();
    if( Editor.FilePath.isEmpty() == true )
        return;

    // TODO: CMDLine 을 이용하여 FileFullPath 를 프로세스에 전달할 명령행으로 변환한다. 
    Editor.CMDLine;

    nsCmn::nsProcess::CreateProcessAsNormal( QDir::toNativeSeparators( Editor.FilePath ), FileFullPath, false, false );
}

void CmpPanel::ChangeVolume( const QString& Drive )
{
    StopEverythingSearch(false);
    int FsIndex = -1;

    for( int idx = 0; idx < ui.cbxVolume->count(); ++idx )
    {
        const auto& Fs = ui.cbxVolume->itemData(idx).value< nsHC::TySpFileSystem >();
        if( Fs == nullptr )
            continue;

        // TODO: Drive 가 UNC 인 경우에도 추가처리가 필요하다. 
        const auto Root = Fs->GetRoot();
        if( Root.compare( Drive, Qt::CaseInsensitive ) != 0 )
            continue;

        FsIndex = idx;
        break;
    }

    if (FsIndex >= 0) {
        ui.cbxVolume->setCurrentIndex(FsIndex);
        changeVolumeIndex(FsIndex);
    }
}

void CmpPanel::ChangeCurrentPath( const QString& Path, const QString& Parameter )
{
    Q_UNUSED(Parameter);
    const auto State = retrieveFocusState(); if (!State || Path.trimmed().isEmpty()) return;
    auto* Model = static_cast<CFSModel*>(State->Model);
    const bool WasSearch = Model->IsSearchResults();
    if (WasSearch) StopEverythingSearch(false);
    if (Model->IsLoading()) Model->CancelNavigation(true);
    const auto Location = Model->WorkspaceLocation();
    emit sig_NotifyPanelActivated();
    Model->setProperty("hc.historyTarget", {});
    if (!WasSearch && Path.trimmed() == CurrentPath()) return;
    if (Location.kind_ == hc::storage::LocatorKind::Archive && Model->GetRoot()) {
        const auto Root = Model->GetRoot()->GetRoot();
        if (Path.startsWith(Root, Qt::CaseInsensitive) && Path.size() > Root.size() && (Path[Root.size()] == '/' || Path[Root.size()] == '\\')) {
            Model->ChangeDirectory(Path.mid(Root.size())); return;
        }
    }
    if ((Location.kind_ == hc::storage::LocatorKind::Plugin || Location.kind_ == hc::storage::LocatorKind::Archive)
        && !(Path.size() >= 3 && Path[1] == ':') && !Path.startsWith("\\\\")) {
        auto VirtualPath = Path;
        if (Location.kind_ == hc::storage::LocatorKind::Plugin && (Path.contains("://") || Path.startsWith("wfx:") || Path.startsWith("dopus:"))) {
            auto Target = Location; Target.root_ = Path.toStdString(); VirtualPath = hc::workspace::PluginPath(Target);
            if (VirtualPath.isEmpty()) return;
        }
        Model->ChangeDirectory(VirtualPath); return;
    }
    auto Target = QDir::toNativeSeparators(Path.trimmed());
    if (QDir::isRelativePath(Target)) Target = QDir(CurrentPath()).absoluteFilePath(Target);
    Target = QDir::toNativeSeparators(QDir::cleanPath(Target));
    auto Saved = CaptureWorkspace().tabs_[ui.tabWidget->currentIndex()];
    Saved.location_ = {Target.startsWith("\\\\") ? hc::storage::LocatorKind::Smb : hc::storage::LocatorKind::Local, Target.toStdString()};
    Saved.provider_.clear(); Saved.nativeRoot_.clear(); Saved.selection_.clear(); Saved.focus_.clear();
    State->workspace_ = Saved; restoreTab(ui.tabWidget->currentIndex());
    updateWorkspaceStatus(); syncPanelChrome();

}

//////////////////////////////////////////////////////////////////////////////
/// Slot Handler 

void CmpPanel::setActivePanel(bool Active)
{
    if (activePanel_ == Active && property("hc.activePanel").isValid()) return;
    activePanel_ = Active;
    setProperty("hc.activePanel", Active);
    for (const auto& State : vecTabStates)
        if (auto* Grid = qobject_cast<CGridView*>(State->View)) Grid->SetPanelActive(Active);
    updateActiveAppearance();
}

void CmpPanel::updateActiveAppearance()
{
    if (tabs_) tabs_->setPaneActive(activePanel_);
    if (path_) path_->SetPaneActive(activePanel_);
}

void CmpPanel::OnColorSchemeChanged( const TyColorScheme& ColorScheme )
{
    darkTheme_ = ColorScheme.IsDarkMode;
    updateActiveAppearance();
    for( const auto& State : vecTabStates )
    {
        if( State->View == nullptr )
            continue;

        State->View->ApplyColorScheme( ColorScheme );
    }

    if (const auto State = retrieveFocusState()) {
        if (State->Model->GetRoot()) processVolumeStatusText(State->Model->GetRoot());
        processPanelStatusText();
    }
}


void CmpPanel::OnChangedDirectory(const QString& CurrentPath)
{
    int Index = ui.tabWidget->currentIndex();
    if (sender()) for (int Candidate = 0; Candidate < vecTabStates.size(); ++Candidate)
        if (vecTabStates[Candidate]->Model == sender()) { Index = Candidate; break; }
    if (Index < 0 || Index >= vecTabStates.size()) return;
    const auto State = vecTabStates[Index]; const auto Model = static_cast<CFSModel*>(State->Model);
    auto Location = Model->WorkspaceLocation();
    if (Location.kind_ == hc::storage::LocatorKind::Plugin) Location.root_ = hc::workspace::PluginPath(Location).toStdString();
    const auto Requested = Model->property("hc.historyTarget");
    hc::workspace::RecordNavigation(State->workspace_, Location, Requested.isValid() ? Requested.toInt() : -1);
    Model->setProperty("hc.historyTarget", {});
    const auto Current = CaptureWorkspace().tabs_[Index];
    State->workspace_.location_ = Current.location_;
    State->workspace_.historyConnections_.insert(hc::workspace::LocationKey(Location), QJsonObject{{"provider", Current.provider_}, {"nativeRoot", Current.nativeRoot_}});
    QSet<QString> Retained;
    for (const auto& Item : State->workspace_.history_) Retained.insert(hc::workspace::LocationKey(Item));
    for (const auto& Key : State->workspace_.historyConnections_.keys())
        if (!Retained.contains(Key)) State->workspace_.historyConnections_.remove(Key);
    // Labels are lexical: no existence/volume probe on the UI thread.
    auto Label = QDir::fromNativeSeparators(CurrentPath).section('/', -1);
    ui.tabWidget->setTabText(Index, Label.isEmpty() ? CurrentPath : Label); ui.tabWidget->setTabToolTip(Index, CurrentPath);
    syncPanelChrome();
    if (Index != ui.tabWidget->currentIndex()) return;
    if (history_) {
        QSignalBlocker Blocker(history_); history_->clear();
        for (const auto& Item : State->workspace_.history_) history_->addItem(QString::fromStdString(Item.root_));
        history_->setCurrentIndex(State->workspace_.historyIndex_);
    }
    processPanelStatusText(); updateWorkspaceStatus(); syncPanelChrome(); emit sig_NotifyCurrentDirectory(CurrentPath);
}


void CmpPanel::changeVolumeIndex( int index, const QString& Path )
{
    StopEverythingSearch(false);
    // TODO: 실제 파일시스템 외의 것들은 예외처리 필요함
    const auto State = retrieveFocusState();
    if( State == nullptr )
        return;

    // 해당 탭에서 QuickView 를 표시하고 있었다면 닫고 GridView 로 전환한다.
    if( State->GetViewMode() == CViewT::VM_QUICK )
        CloseQuickView( retrieveCurrentIndex() );

    const auto ViewMode = State->View->GetViewMode();
    Q_ASSERT( ViewMode == CViewT::VM_GRID );

    const auto Fs = ui.cbxVolume->itemData(index).value<nsHC::TySpFileSystem>();
    if (!Fs) return;
    State->Model->setProperty("hc.historyTarget", {});
    emit sig_NotifyPanelActivated();

    State->Model->SetRoot( Fs );
    State->Model->SetCurrentPath( "\\" );
    if( Path.isEmpty() == true )
        State->Model->ChangeDirectory( QModelIndex() );
    else
        State->Model->ChangeDirectory( Path );

    State->View->AdjustSizeToContents();
    QTimer::singleShot( 0, [State]() {State->View->EnsureKeyboardFocusOnView(); } );

    processVolumeStatusText( Fs );
}


void CmpPanel::on_tabWidget_currentChanged(int Index)
{
    if (Index < 0 || Index >= vecTabStates.size()) return;
    emit sig_NotifyPanelActivated();
    const auto State = vecTabStates[Index];
    if (State->pending_) restoreTab(Index);
    for (int Candidate = 0; Candidate < vecTabStates.size(); ++Candidate)
    {
        if (Candidate != Index) static_cast<CFSModel*>(vecTabStates[Candidate]->Model)->StopEverythingSearch();
        static_cast<CFSModel*>(vecTabStates[Candidate]->Model)->SetWatchEnabled(Candidate == Index);
    }
    if (search_) { QSignalBlocker Blocker(search_); search_->setText(State->ProxyModel->Search()); }
    if (history_) {
        QSignalBlocker Blocker(history_); history_->clear();
        for (const auto& Location : State->workspace_.history_) history_->addItem(QString::fromStdString(Location.root_));
        history_->setCurrentIndex(State->workspace_.historyIndex_);
    }
    updateWorkspaceStatus();
    syncPanelChrome();
    processVolumeStatusText(State->Model->GetRoot());
    emit sig_NotifyCurrentDirectory(CurrentPath());
}



void CmpPanel::resizeEvent( QResizeEvent* event )
{
    QWidget::resizeEvent( event );
}

bool CmpPanel::eventFilter(QObject* Object, QEvent* Event)
{
    if (Object == path_ && (Event->type() == QEvent::Resize || Event->type() == QEvent::Show)) updateNavigationProgress();
    return QWidget::eventFilter(Object, Event);
}

void CmpPanel::updateNavigationProgress()
{
    if (!navigationProgress_ || !path_) return;
    const auto State = vecTabStates.value(ui.tabWidget->currentIndex());
    const auto Model = State ? static_cast<CFSModel*>(State->Model) : nullptr;
    const auto View = State ? static_cast<CGridView*>(State->View) : nullptr;
    const bool ColorsPending = Model && View && !Model->AreFileColorsReady(View->VisibleSourceRows());
    const bool Busy = Model && (Model->IsLoading() || ColorsPending);
    navigationProgress_->setGeometry(2, qMax(0, path_->height() - 4), qMax(0, path_->width() - 4), 3);
    navigationProgress_->setVisible(Busy);
    if (Busy) {
        navigationProgress_->setAccessibleName(ColorsPending && !Model->IsLoading() ? tr("파일 색상 적용 중…") : State->navigationStatus_);
        navigationProgress_->setToolTip(State->navigationStatus_ + "\n" + QDir::toNativeSeparators(State->navigationTarget_));
        navigationProgress_->raise();
    }
}


void CmpPanel::initializeUIs()
{
    setMinimumWidth(240);
    ui.verticalLayout->setContentsMargins(3, 0, 3, 0);
    ui.verticalLayout->setSpacing(2);
    ui.lblVolumeStatus->setMinimumWidth(0);
    ui.lblVolumeStatus->setMaximumWidth(QWIDGETSIZE_MAX);
    ui.lblVolumeStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    ui.lblVolumeStatus->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    ui.btnGridStyle->hide(); ui.cbxVolume->hide();
    ui.tabWidget->tabBar()->hide();
    ui.frmFooter->hide();
    tabs_ = new fm::ui::PanelTabStrip(this); tabs_->setObjectName("hc.panelTabs");
    tabs_->tabBar()->setElideMode(Qt::ElideRight);
    ui.verticalLayout->insertWidget(0, tabs_);
    drive_ = new fm::ui::DriveButton(ui.frmHDR); drive_->setObjectName("hc.panelDrive");
    auto* Drives = new QMenu(drive_); drive_->setMenu(Drives);
    connect(Drives, &QMenu::aboutToShow, this, [this, Drives] {
        RefreshVolumeList(); Drives->clear();
        for (int Index = 0; Index < ui.cbxVolume->count(); ++Index) {
            auto* Action = Drives->addAction(ui.cbxVolume->itemIcon(Index), ui.cbxVolume->itemText(Index));
            const auto Fs = ui.cbxVolume->itemData(Index).value<nsHC::TySpFileSystem>();
            const auto Root = Fs ? Fs->GetRoot() : QString{};
            connect(Action, &QAction::triggered, this, [this, Root] { ChangeVolume(Root); });
        }
        syncPanelChrome();
    });
    ui.horizontalLayout->insertWidget(0, drive_);
    ui.horizontalLayout->setSpacing(4);
    // Inset the drive row like the tab strip so the drive button lines up under the first tab
    // instead of jutting out at the panel edge. The strip updates its own inset first on theme changes.
    const auto AlignHeader = [this] {
        const auto Inset = tabs_->layout()->contentsMargins();
        ui.horizontalLayout->setContentsMargins(Inset.left(), 0, Inset.right(), 0);
    };
    AlignHeader();
    connect(&fm::style::ThemeManager::instance(), &fm::style::ThemeManager::changed, this, AlignHeader);
    ui.horizontalLayout->setStretch(2, 1);
    viewMode_ = new fm::ui::SegmentedControl({tr("1줄"), tr("2줄"), tr("자동"), tr("썸네일")}, ui.frmHDR);
    viewMode_->setObjectName("hc.panelViewMode"); viewMode_->setSegmentSize(fm::ui::SegmentedControl::Compact);
    viewMode_->setCurrentIndex(0);
    viewMode_->setItemToolTips({tr("한 줄 목록"), tr("이름 우선 두 줄 목록"), tr("설정의 패널 폭과 잘린 이름 비율로 1줄/2줄을 전환합니다."), tr("이미지 타일 보기 · Ctrl+휠로 크기를 변경합니다.")});
    ui.horizontalLayout->insertWidget(ui.horizontalLayout->count() - 1, viewMode_);
    connect(viewMode_, &fm::ui::SegmentedControl::currentIndexChanged, this, [this](int Index) {
        if (Index < 0 || Index > 3) return;
        emit sig_NotifyPanelActivated();
        if (const auto State = retrieveFocusState()) {
            auto* View = static_cast<CGridView*>(State->View);
            if (!View->SetListMode(QStringList{"one-line", "two-line", "auto", "thumbnails"}.value(Index))) syncPanelChrome();
        }
    });
    auto* Controls = new QWidget(this); auto* Layout = new QHBoxLayout(Controls);
    Layout->setContentsMargins(0, 0, 0, 0); Layout->setSpacing(2);
    path_ = new hc::ui::PanelPathBar(Controls); path_->setObjectName("hc.panelPath");
    connect(path_, &hc::ui::PanelPathBar::activated, this, &CmpPanel::sig_NotifyPanelActivated);
    history_ = new QComboBox(this); history_->setObjectName("hc.workspaceHistory"); history_->hide();
    historyButton_ = new QToolButton(Controls); historyButton_->setText(QStringLiteral("▾"));
    historyButton_->setObjectName("hc.panelHistory"); historyButton_->setToolTip(tr("방문 기록"));
    historyButton_->setFixedSize(24, 24);
    historyButton_->setAccessibleName(tr("방문 기록")); historyButton_->setPopupMode(QToolButton::InstantPopup);
    auto* HistoryMenu = new QMenu(historyButton_); historyButton_->setMenu(HistoryMenu);
    connect(history_, &QComboBox::activated, this, &CmpPanel::navigateHistoryIndex);
    connect(HistoryMenu, &QMenu::aboutToShow, this, [this, HistoryMenu] {
        HistoryMenu->clear(); const auto State = retrieveFocusState();
        for (int Index = int(State->workspace_.history_.size()) - 1; Index >= 0; --Index) {
            auto* Action = HistoryMenu->addAction(QString::fromStdString(State->workspace_.history_[Index].root_));
            Action->setCheckable(true); Action->setChecked(Index == State->workspace_.historyIndex_);
            connect(Action, &QAction::triggered, this, [this, Index] { navigateHistoryIndex(Index); });
        }
    });
    Layout->addWidget(path_, 1); Layout->addWidget(historyButton_);
    navigationProgress_ = new fm::ui::ProgressBar(path_);
    navigationProgress_->setObjectName("hc.navigation.progress");
    navigationProgress_->setThickness(fm::ui::ProgressBar::ThinBar); navigationProgress_->setCompact(true);
    navigationProgress_->setRange(0, 0); navigationProgress_->setTextVisible(false); navigationProgress_->setFixedHeight(3);
    navigationProgress_->setFocusPolicy(Qt::NoFocus); navigationProgress_->setAttribute(Qt::WA_TransparentForMouseEvents);
    navigationProgress_->hide(); path_->installEventFilter(this);
    ui.verticalLayout->insertWidget(2, Controls);
    search_ = new QLineEdit(this); search_->setObjectName("hc.workspaceSearch"); search_->hide();
    connect(search_, &QLineEdit::textChanged, this, [this](const QString& Text) { SetSearch(Text); });
    connect(path_, &fm::ui::BreadcrumbBar::pathEntered, this, [this](const QString& Path) {
        emit sig_NotifyPanelActivated(); ChangeCurrentPath(Path, {}); SetFocusView(CurrentTabIndex());
    });
    connect(tabs_, &fm::ui::PanelTabStrip::newTabRequested, this, [this] { emit sig_NotifyPanelActivated(); AddTab(); SetFocusView(CurrentTabIndex()); });
    connect(tabs_, &fm::ui::PanelTabStrip::closeTabRequested, this, &CmpPanel::closeTabAt);
    connect(tabs_->tabBar(), &QTabBar::currentChanged, this, [this](int Index) {
        const auto Id = tabs_->tabBar()->tabData(Index).toString();
        for (int Candidate = 0; Candidate < vecTabStates.size(); ++Candidate)
            if (vecTabStates[Candidate]->workspace_.id_ == Id) { ui.tabWidget->setCurrentIndex(Candidate); break; }
    });
    connect(tabs_->tabBar(), &QTabBar::tabMoved, this, [this](int From, int To) {
        { QSignalBlocker Blocker(ui.tabWidget);
          ui.tabWidget->tabBar()->moveTab(From, To); vecTabStates.move(From, To); }
        on_tabWidget_currentChanged(ui.tabWidget->currentIndex()); emit sigViewSettingsChanged();
    });
    auto* Notice = new QWidget(this); auto* NoticeLayout = new QHBoxLayout(Notice); NoticeLayout->setContentsMargins(0, 0, 0, 0);
    workspaceStatus_ = new QLabel(Notice); workspaceStatus_->setWordWrap(true); workspaceStatus_->setTextFormat(Qt::PlainText);
    retry_ = new QPushButton(tr("다시 시도"), Notice); retry_->setObjectName("hc.workspaceRetry");
    NoticeLayout->addWidget(workspaceStatus_, 1); NoticeLayout->addWidget(retry_); ui.verticalLayout->insertWidget(3, Notice);
    Notice->hide();
    connect(retry_, &QPushButton::clicked, this, [this] { RefreshSource(ui.tabWidget->currentIndex()); });
    status_ = new fm::ui::PanelStatusBar(this); status_->setObjectName("hc.panelStatus"); ui.verticalLayout->addWidget(status_);
    AddTab(false);
}

QString CmpPanel::CurrentPath() const
{
    const int Index = ui.tabWidget->currentIndex();
    if (Index < 0 || Index >= vecTabStates.size()) return {};
    const auto State = vecTabStates[Index];
    const auto Location = State->pending_ ? State->workspace_.location_ : static_cast<CFSModel*>(State->Model)->WorkspaceLocation();
    const auto* Model = static_cast<CFSModel*>(State->Model);
    return State->pending_ || Model->HasPlaceholder() ? QString::fromStdString(Location.root_) : Model->GetCurrentPathWithRoot();
}
QString CmpPanel::Search() const
{
    const int Index = ui.tabWidget->currentIndex();
    return Index >= 0 && Index < vecTabStates.size() ? vecTabStates[Index]->ProxyModel->Search() : QString{};
}
QString CmpPanel::SearchMatch() const
{
    const int Index = ui.tabWidget->currentIndex();
    return Index >= 0 && Index < vecTabStates.size() ? vecTabStates[Index]->ProxyModel->SearchMatch() : QString{};
}
void CmpPanel::SetSearch(const QString& Query, const QString& Match)
{
    const int Index = ui.tabWidget->currentIndex(); if (Index < 0 || Index >= vecTabStates.size()) return;
    vecTabStates[Index]->ProxyModel->SetSearch(Query, Match);
    { QSignalBlocker Blocker(search_); search_->setText(Query); }
    processPanelStatusText(); emit sigNavigationStateChanged();
}
void CmpPanel::StartEverythingSearch(const QString& Query, const QString& Match, int Interval)
{
    const auto State = retrieveFocusState(); if (!State || Query.isEmpty()) return;
    auto* Model = static_cast<CFSModel*>(State->Model);
    if (!State->searchOrigin_) {
        State->searchOrigin_ = CaptureWorkspace().tabs_[CurrentTabIndex()];
        auto* View = static_cast<CGridView*>(State->View);
        State->searchScroll_ = View->CaptureScrollState();
        State->searchGroups_ = View->CaptureGroupExpansion();
        View->SetEverythingPresentation(true);
        auto Profile = View->ColumnProfile();
        auto Found = std::find_if(Profile.columns_.begin(), Profile.columns_.end(), [](const auto& C) { return C.fieldId_ == "file.parentPath"; });
        hc::columns::ColumnDefinition Location;
        if (Found != Profile.columns_.end()) { Location = *Found; Profile.columns_.erase(Found); }
        Location.fieldId_ = "file.parentPath"; Location.title_ = tr("경로");
        for (auto& Width : Location.widths_) { Width.hidden_ = false; Width.unit_ = hc::view::WidthUnit::Percent; Width.value_ = 60; Width.minimum_ = 20; Width.maximum_ = 100; }
        Profile.columns_.insert(1, Location); Profile.mode_ = hc::columns::LayoutMode::NameLast;
        View->ApplyColumnProfile(Profile);
    }
    State->ProxyModel->SetSearch({});
    { const QSignalBlocker Block(search_); search_->clear(); }
    Model->StartEverythingSearch(Query, Match, Interval);
    processPanelStatusText();
}

void CmpPanel::StopEverythingSearch(bool Restore)
{
    const auto State = retrieveFocusState(); if (!State || !State->searchOrigin_) return;
    const auto Origin = *State->searchOrigin_; State->searchOrigin_.reset();
    static_cast<CFSModel*>(State->Model)->StopEverythingSearch(true);
    auto* View = static_cast<CGridView*>(State->View);
    View->SetEverythingPresentation(false);
    const auto Presentation = View->CaptureWorkspaceView().value("listPresentation");
    State->workspace_ = Origin;
    auto Settings = Origin.view_; Settings["listPresentation"] = Presentation;
    View->RestoreWorkspaceView(Settings);
    if (Restore) { State->restoreSearchGroups_ = true; State->restoreSelection_ = true; restoreTab(CurrentTabIndex()); }
    else { State->searchScroll_ = {}; State->searchGroups_ = {}; State->restoreSearchGroups_ = false; }
    syncPanelChrome();
}

void CmpPanel::OpenSearchLocation()
{
    const auto State = retrieveFocusState(); if (!State || !static_cast<CFSModel*>(State->Model)->IsSearchResults()) return;
    auto* View = static_cast<CGridView*>(State->View);
    const auto Focus = View->CaptureFocus();
    for (int Row = 0; Row < State->Model->rowCount(); ++Row) {
        const auto Item = State->Model->GetFileInfo(State->Model->index(Row, 0));
        if (!Item || !Item->storageEntry_ || hc::workspace::SelectionKey(*Item->storageEntry_) != Focus) continue;
        const auto Entry = *Item->storageEntry_;
        const auto Parent = QFileInfo(QString::fromStdString(Entry.locator_.root_)).absolutePath();
        ChangeCurrentPath(Parent, {});
        State->workspace_.selection_ = {Focus}; State->workspace_.focus_ = Focus; State->restoreSelection_ = true; break;
    }
}

bool CmpPanel::CanNavigateHistory(int Offset) const
{
    const int Index = ui.tabWidget->currentIndex(); if (Index < 0 || Index >= vecTabStates.size()) return false;
    const auto State = vecTabStates[Index]; const int Target = State->workspace_.historyIndex_ + Offset;
    return !static_cast<CFSModel*>(State->Model)->IsLoading() && Target >= 0 && Target < State->workspace_.history_.size();
}
void CmpPanel::NavigateHistory(int Offset)
{
    if (CanNavigateHistory(Offset)) navigateHistoryIndex(retrieveFocusState()->workspace_.historyIndex_ + Offset);
}
void CmpPanel::navigateHistoryIndex(int Index)
{
    const auto State = retrieveFocusState();
    if (State->searchOrigin_) StopEverythingSearch(false);
    if (Index < 0 || Index >= State->workspace_.history_.size() || static_cast<CFSModel*>(State->Model)->IsLoading()) return;
    emit sig_NotifyPanelActivated();
    auto Saved = CaptureWorkspace().tabs_[ui.tabWidget->currentIndex()]; Saved.location_ = State->workspace_.history_[Index];
    const auto Connection = Saved.historyConnections_.value(hc::workspace::LocationKey(Saved.location_)).toObject();
    Saved.provider_ = Connection.value("provider").toString(); Saved.nativeRoot_ = Connection.value("nativeRoot").toString();
    Saved.selection_.clear(); Saved.focus_.clear(); State->workspace_ = Saved;
    State->Model->setProperty("hc.historyTarget", Index); restoreTab(ui.tabWidget->currentIndex());
}
void CmpPanel::syncPanelChrome()
{
    if (!tabs_) return;
    auto* Bar = tabs_->tabBar(); QSignalBlocker Blocker(Bar);
    while (Bar->count() > vecTabStates.size()) Bar->removeTab(Bar->count() - 1);
    for (int Index = 0; Index < vecTabStates.size(); ++Index) {
        if (Index >= Bar->count()) Bar->addTab(ui.tabWidget->tabText(Index));
        Bar->setTabText(Index, ui.tabWidget->tabText(Index)); Bar->setTabToolTip(Index, ui.tabWidget->tabToolTip(Index));
        Bar->setTabData(Index, vecTabStates[Index]->workspace_.id_);
        vecTabStates[Index]->Index = Index;
    }
    Bar->setCurrentIndex(ui.tabWidget->currentIndex());
    Bar->setMaximumWidth(std::max(30, Bar->sizeHint().width()));
    const auto* SearchModel = static_cast<CFSModel*>(retrieveFocusState()->Model);
    const auto Kind = SearchModel->WorkspaceLocation().kind_;
    path_->SetBreadcrumbsEnabled(!SearchModel->IsSearchResults()
        && (Kind == hc::storage::LocatorKind::Local || Kind == hc::storage::LocatorKind::Smb));
    path_->setFullPath(SearchModel->IsSearchResults() ? tr("Everything · %1").arg(SearchModel->EverythingQuery()) : CurrentPath());
    { QSignalBlocker ViewBlocker(viewMode_);
      auto* View = static_cast<CGridView*>(retrieveFocusState()->View);
      viewMode_->setCurrentIndex(QStringList{"one-line", "two-line", "auto", "thumbnails"}.indexOf(View->ListMode()));
      viewMode_->button(2)->setToolTip(tr("자동 → %1 · 패널 폭과 잘린 이름 비율 기준").arg(View->IsTwoLine() ? tr("2줄") : tr("1줄"))); }
    const auto Fs = ui.cbxVolume->currentData().value<nsHC::TySpFileSystem>();
    drive_->setText(Fs ? Fs->GetRoot() : tr("위치"));
    historyButton_->setEnabled(!retrieveFocusState()->workspace_.history_.isEmpty());
    emit sigNavigationStateChanged();
}

CViewT* CmpPanel::createGridView()
{
    CGridView* View = new CGridView;
    View->layout()->setContentsMargins(0, 0, 0, 0);
    View->SetPanelActive(activePanel_);

    return View;
}

int CmpPanel::retrieveCurrentIndex() const
{
    return ui.tabWidget->currentIndex();
}

CmpPanel::TySpTabState CmpPanel::retrieveFocusState()
{
    const auto Index = ui.tabWidget->currentIndex();
    Q_ASSERT( Index >= 0 );
    
    return vecTabStates[ Index ];
}

CViewT* CmpPanel::retrieveFocusView() const
{
    const auto Current = retrieveCurrentIndex();
    if( Current < 0 || Current >= vecTabStates.size() )
        return {};

    const auto& State = vecTabStates.value( Current );
    if( State->QuickView != nullptr )
        return State->QuickView;

    return State->View;
}

QPoint CmpPanel::retrieveMenuPoint( const QPoint& GlobalCursor, QModelIndex SrcIndex )
{
    QPoint Pos( GlobalCursor );
    const auto View = retrieveFocusView();

    do
    {
        if( View->GetViewMode() != CViewT::VM_GRID )
            break;
        
        const auto GridViewBase = qobject_cast< CGridView* >( View )->BaseView();
        const auto Row = GridViewBase->getRow( SrcIndex );

        if( SrcIndex.isValid() == false || Row.isValid() == false )
            return Pos;

        QRect GlobalRowRect;
        const auto Grid = GridViewBase->grid();
       
        for( const auto& Info : Grid->hitInfoAll() )
        {
            if( Info.info() != GridHitInfo::Cell )
                continue;

            if( Info.row().rowIndex() != Row.rowIndex() )
                continue;

            if( Info.columnIndex() == 0 )
            {
                GlobalRowRect.setTopLeft( Grid->mapToGlobal( Info.rect().topLeft() ) );
                GlobalRowRect.setBottomRight( Grid->mapToGlobal( Info.rect().bottomRight() ) );
            }

            if( Info.columnIndex() == GridViewBase->getColumnCount() - 1 )
                GlobalRowRect.setBottomRight( Grid->mapToGlobal( Info.rect().bottomRight() ) );
        }

        do
        {
            if( Pos.isNull() == true )
            {
                Pos = GlobalRowRect.topLeft();
                break;
            }

            if( GlobalRowRect.contains( Pos ) == true )
                break;

            Pos = GlobalRowRect.topLeft();
            break;

        } while( false );

    } while( false );

    return Pos;
}

QVector<QModelIndex> CmpPanel::makeSrcModel( const TySpTabState& SrcState )
{
    QVector<QModelIndex> SrcModel;
    const auto View = qobject_cast< CGridView* >( SrcState->View );

    do
    {
        if( View == nullptr )
            break;

        const auto BaseGrid = View->BaseView();
        if( BaseGrid == nullptr )
            break;

        for (const auto& Index : View->SelectedSourceIndexes())
        {
            if( SrcState->Model->GetName( Index ) == ".." )
                continue;

            SrcModel.push_back( Index );
        }

        // 선택된 항목이 없다면 현재 커서가 위치한 항목을 복사한다. 
        if( SrcModel.isEmpty() == true )
        {
            const auto Row = BaseGrid->focusedRow();
            if (Row.isValid() && Row.type() == Qtitan::GridRow::DataRow)
            {
                const auto Index = SrcState->ConvertToSrcIndex( Row.modelIndex( 0 ) );
                if( SrcState->Model->GetName( Index ) != ".." )
                {
                    SrcModel.push_back( Index );
                }
            }
        }

    } while( false );

    return SrcModel;
}

void CmpPanel::processVolumeStatusText(const nsHC::TySpFileSystem& SpFileSystem)
{
    const auto Current = retrieveFocusState();
    const auto* CurrentModel = Current ? static_cast<CFSModel*>(Current->Model) : nullptr;
    if (CurrentModel && CurrentModel->IsAutomaticRefresh()
        && !hc::refresh::Settings::FromJson(hc::settings::AppSettings().CommittedObject()["general"].toObject()["refresh"].toObject()).updateHeader) return;
    const auto Generation = ++capacityGeneration_;
    ui.lblVolumeStatus->setToolTip({});
    if (!SpFileSystem) { ui.lblVolumeStatus->clear(); return; }
    const auto Root = SpFileSystem->GetRoot();
    const auto Cate = SpFileSystem->GetCate();
    if (Cate != nsHC::FS_CATE_LOCAL && Cate != nsHC::FS_CATE_REMOTE) { ui.lblVolumeStatus->setText(Root); return; }
    const auto State = retrieveFocusState();
    const auto* Model = State ? static_cast<CFSModel*>(State->Model) : nullptr;
    const auto Directory = QDir::toNativeSeparators(CurrentPath());
    const bool Remote = SpFileSystem->IsRemote();
    auto Volume = Root;
    if (Remote)
    {
        ui.lblVolumeStatus->setText(tr("[%1] 네트워크 드라이브").arg(Directory.isEmpty() ? Root : Directory));
        // Probe only a successfully opened location. Drive enumeration and failed
        // navigation must not reconnect an unavailable mapping for metadata.
        if (!Model || State->pending_ || Model->GetRoot() != SpFileSystem || Model->IsLoading()
            || Model->HasPlaceholder() || Model->LastNavigationError()) return;
        auto Unc = Directory;
        if (Unc.startsWith("\\\\?\\UNC\\", Qt::CaseInsensitive)) Unc = "\\\\" + Unc.mid(8);
        // A server lists shares; only a share (or a child) has disk capacity.
        if (Unc.startsWith("\\\\"))
        {
            const auto Parts = Unc.mid(2).split('\\', Qt::SkipEmptyParts);
            if (Parts.size() < 2) return;
            Volume = "\\\\" + Parts[0] + '\\' + Parts[1];
        }
    }
    const auto Name = Remote ? Volume : SpFileSystem->GetVolumeName();
    const auto QueryPath = Remote ? Directory : Root;
    ui.lblVolumeStatus->setText(tr("[%1] 용량 조회 중").arg(Name.isEmpty() ? Root : Name));
    auto* Watcher = new QFutureWatcher<QPair<quint64, quint64>>(this);
    connect(Watcher, &QFutureWatcherBase::finished, this, [this, Watcher, Generation, SpFileSystem, Directory, Root, Name] {
        const auto Value = Watcher->result(); Watcher->deleteLater();
        const auto State = retrieveFocusState();
        if (Generation != capacityGeneration_ || !State || State->pending_ || State->Model->GetRoot() != SpFileSystem
            || Directory.compare(QDir::toNativeSeparators(CurrentPath()), Qt::CaseInsensitive) != 0) return;
        const auto SizeStyle = static_cast<TyEnSizeStyle>(hc::settings::AppSettings().SizeStyle(true));
        ui.lblVolumeStatus->setText(Value.second ? tr("[%1] 여유 %2 / %3").arg(Name.isEmpty() ? Root : Name,
            GetFormattedSizeText(Value.first, SizeStyle), GetFormattedSizeText(Value.second, SizeStyle)) : tr("[%1] 용량 확인 불가").arg(Name.isEmpty() ? Root : Name));
        ui.lblVolumeStatus->setToolTip(ui.lblVolumeStatus->text());
    });
    Watcher->setFuture(QtConcurrent::run([QueryPath] {
        ULARGE_INTEGER Available{}, Total{};
        // UNC paths require a trailing backslash, including share roots.
        if (!GetDiskFreeSpaceExW((QueryPath.endsWith('\\') ? QueryPath : QueryPath + '\\').toStdWString().c_str(), &Available, &Total, nullptr))
            return qMakePair(quint64(0), quint64(0));
        return qMakePair(quint64(Available.QuadPart), quint64(Total.QuadPart));
    }));
}

void CmpPanel::processPanelStatusText()
{
    if (ui.tabWidget->currentIndex() < 0 || ui.tabWidget->currentIndex() >= vecTabStates.size()) return;
    const auto& State = retrieveFocusState();
    const bool Automatic = State->automaticSnapshot_ || static_cast<CFSModel*>(State->Model)->IsAutomaticRefresh();
    const auto Refresh = hc::refresh::Settings::FromJson(hc::settings::AppSettings().CommittedObject()["general"].toObject()["refresh"].toObject());
    if (!State->footerValid_ || !Automatic || Refresh.updateFooter) {
        State->footerTotalSize_ = State->Model->GetTotalSize();
        State->footerFileCount_ = State->Model->GetFileCount();
        State->footerDirectoryCount_ = State->Model->GetDirectoryCount();
        State->footerValid_ = true;
    }
    const auto SizeStyle = static_cast< TyEnSizeStyle >( hc::settings::AppSettings().SizeStyle(false) );

    ui.lblStatus->setText( QString( "크기: %1 / %2\t파일: %3 / %4\t폴더: %5 / %6" )
                           .arg( GetFormattedSizeText( State->SelectedSize, SizeStyle ) ).arg( GetFormattedSizeText( State->footerTotalSize_, SizeStyle ) )
                           .arg( State->SelectedFileCount ).arg( State->footerFileCount_ )
                           .arg( State->SelectedDirectoryCount ).arg( State->footerDirectoryCount_ ) );
    if (status_) {
        const auto* SearchModel = static_cast<CFSModel*>(State->Model);
        if (SearchModel->IsSearchResults()) {
            status_->setLeftText(SearchModel->EverythingStatus());
            status_->setRightText(tr("%1개 선택").arg(State->SelectedFileCount + State->SelectedDirectoryCount));
            return;
        }
        const auto Model = static_cast<CFSModel*>(State->Model);
        const auto First = Model->rowCount() ? Model->GetFileInfo(Model->index(0, 0)) : nullptr;
        const int ParentCount = First && First->Name_ == ".." ? 1 : 0;
        const int FolderCount = std::max(0, State->footerDirectoryCount_ - ParentCount);
        status_->setLeftText(Model->IsLoading() ? (State->navigationStatus_.isEmpty() ? tr("목록 조회 중…") : State->navigationStatus_) : Model->LastNavigationError()
            ? (Model->LastNavigationError()->category_ == hc::storage::ErrorCategory::Cancelled ? tr("목록 조회 취소 · 다시 시도하십시오") : tr("목록 조회 실패 · 다시 시도하십시오")) :
            tr("파일 %1 / %2개 선택 · %3 / %4").arg(State->SelectedFileCount).arg(State->footerFileCount_)
            .arg(GetFormattedSizeText(State->SelectedSize, SizeStyle), GetFormattedSizeText(State->footerTotalSize_, SizeStyle)));
        status_->setRightText(Search().isEmpty() ? tr("폴더 %1개").arg(FolderCount) : tr("검색 결과 %1개").arg(std::max(0, State->ProxyModel->rowCount() - ParentCount)));
        status_->setToolTip(ui.lblStatus->text());
    }
}

//void CmpPanel::Initialize()
//{
//    //ui.cbxVolume->setEditable( true );
//    //ui.cbxVolume->lineEdit()->setReadOnly( true );
//
//    const auto ColorScheme = TyStColorSchemeMgr::GetInstance()->GetColorScheme( TyStColorSchemeMgr::GetInstance()->GetCurrentColorScheme() );
//
//    sltColorSchemeChanged( ColorScheme );
//
//    //connect( &viewClickTimer, &QTimer::timeout, [this]() {
//    //    const auto View = retrieveFocusView();
//    //    if( View == nullptr )
//    //        return;
//
//    //    View->showEditor( GridEditor::ActivateByClick );
//    //} );
//
//    initializeVolumeList();
//
//
//    //AddTab();
//}
//
//void CmpPanel::sltColorSchemeChanged( const TyColorScheme& ColorScheme )
//{
//    auto Palette = ui.cbxVolume->palette();
//    ui.cbxVolume->setFont( ColorScheme.Dialog_Font );
//
//    Palette.setColor( ui.cbxVolume->backgroundRole(), ColorScheme.Dialog_BGColor );
//    Palette.setColor( QPalette::Base, ColorScheme.Dialog_BGColor );
//    Palette.setColor( ui.cbxVolume->foregroundRole(), ColorScheme.Dialog_FGColor );
//
//    // TODO: 현재 있는 그리드들에 색상 / 글꼴 적용
//}
//

//
////Qtitan::GridBandedTableView* CmpPanel::GetFocusView() const
////{
////    return retrieveFocusView();
////}
////
////


////void CmpPanel::FileNormalization( const QModelIndex& SrcIndex )
////{
////    const auto State = retrieveFocusState();
////    const auto ModelIndex = State->ProxyModel->mapToSource( SrcIndex );
////    const auto Node = State->Model->GetFileInfo( ModelIndex );
////
////    if( Node.IsNormalizedByNFD == FALSE )
////        return;
////
////    const DWORD Ret = State->Model->Rename( ModelIndex, Node.Name.normalized( QString::NormalizationForm_C ) );
////    if( Ret != ERROR_SUCCESS )
////    {
////        // TODO: 오류처리, 오류창 표시
////        // 관리자 권한으로 재시도할 수 있음
////        return;
////    }
////
////    State->Model->setData( ModelIndex, Node.Name.normalized( QString::NormalizationForm_C ), Qt::EditRole );
////}
////
////void CmpPanel::RenameFileName( const QModelIndex& SrcIndex )
////{
////    UNREFERENCED_PARAMETER( SrcIndex );
////
////    viewClickTimer.stop();
////
////    for( const auto& State : vecTabStates )
////        State->LastFocusedRowIndex = -1;
////
////    const auto View = retrieveFocusView();
////    View->setFocusedColumnIndex( 0 );
////    View->showEditor( GridEditor::ActivateByKeyPress_F2 );
////}
////

////int CmpPanel::InitializeGrid()
////{
////    const auto Page = new QWidget;
////    const auto Vertical = new QVBoxLayout;
////    Page->setLayout( Vertical );
////    Page->setSizePolicy( QSizePolicy( QSizePolicy::Policy::Expanding, QSizePolicy::Policy::Expanding ) );
////
////    const auto Grid = new Qtitan::Grid;
////    Grid->setViewType( Qtitan::Grid::BandedTableView );
////    Grid->setSizePolicy( QSizePolicy( QSizePolicy::Policy::Expanding, QSizePolicy::Policy::Expanding ) );
////    Grid->setAttribute( Qt::WA_KeyCompression );
////    Grid->setAttribute( Qt::WA_InputMethodEnabled );
////    Grid->viewport()->setAttribute( Qt::WA_KeyCompression );
////    Grid->viewport()->setAttribute( Qt::WA_InputMethodEnabled );
////
////    Vertical->addWidget( Grid );
////
////    // TODO: 탭 이름 => 현재 디렉토리, 만약 루트 디렉토리라면 드라이브 문자 함께 표시
////    const auto currentIndex = ui.tabWidget->addTab( Page, tr("D:/") );
////    ui.tabWidget->setCurrentIndex( currentIndex );
////
////    Grid->installEventFilter( this );
////    Grid->setContextMenuPolicy( Qt::CustomContextMenu );
////
////    const auto View = Grid->view< GridBandedTableView >();
////    Grid->viewport()->setFocusPolicy( Qt::StrongFocus );
////
////    View->beginUpdate();
////    auto& BaseOpts = View->options();
////    auto& TableOpts = View->tableOptions();
////    auto& BandedOpts = View->bandedOptions();
////
////    BaseOpts.setShowWaitCursor( true );
////    BaseOpts.setGroupsHeader( false );
////    BaseOpts.setScrollBarsTransparent( true );
////    BaseOpts.setMainMenuDisabled( false );              // true 이면 
////    BaseOpts.setAutoCreateColumns( false );
////    BaseOpts.setColumnMovingEnabled( false );
////    BaseOpts.setAlternatingRowColors( false );
////    BaseOpts.setScrollRowStyle( Qtitan::ScrollByPixel );
////    BaseOpts.setGridLines( Qtitan::LinesNone );
////    BaseOpts.setModelDecoration( true );
////    BaseOpts.setModelItemsDragEnabled( true );
////    BaseOpts.setRowStyle( Qtitan::GridViewOptions::RowStyleNormal );
////    BaseOpts.setSelectedDecorationOpacity( 0.2 );
////    BaseOpts.setSelectionPolicy( Qtitan::GridViewOptions::IgnoreSelection );
////    BaseOpts.setFieldChooserEnabled( false );
////    BaseOpts.setRubberBandSelection( false );
////    BaseOpts.setTransparentBackground( true );
////    BaseOpts.setBackgroundColor( QColor( "cyan" ) );
////    BaseOpts.setSelectedBackgroundColor( QColor( 255, 112, 43 ) );
////    BaseOpts.setSelectedInverseColor( true );
////    BaseOpts.setModelDecorationOpacityRole( FSModel::USR_ROLE_HIDDENOPACITY );
////    BaseOpts.setRowAutoHeight( true );
////
////    BaseOpts.setGroupsHeaderText( "Fdsfds" );
////    BaseOpts.setColumnHeaderHints( true );
////    
////    BaseOpts.setFocusFollowsMouse( false );
////    BaseOpts.setFocusFrameEnabled( false );
////    BaseOpts.setShowFocusDecoration( true );
////    BaseOpts.setKeepFocusedRow( true );
////    BaseOpts.setRowFocusMode( true );
////    BaseOpts.setRowFocusColor( QColor( 51, 79, 102 ) );
////    
////    BaseOpts.setDragEnabled( true );
////    BaseOpts.setDropEnabled( true );
////    BaseOpts.setFieldChooserEnabled( false );
////    BaseOpts.setGroupsHeaderTextColor( "white" );
////    
////    TableOpts.setRowsQuickSelection( false );
////    TableOpts.setRowFrozenButtonVisible( false );
////    TableOpts.setColumnsQuickCustomization( false );
////    
////    BandedOpts.setBandsHeader( false );
////    BandedOpts.setBandsQuickCustomization( false );
////    
////
////    const auto StColumnMgr = new CColumnMgr;
////    StColumnMgr->Initialize();
////    const auto ColumnView = StColumnMgr->GetColumnView( 0 );
////
////    const auto Model = new FSModel;
////    const auto ProxyModel = new FSProxyModel;
////
////    Model->SetRoot( "C:" );
////    Model->SetCurrentPath( "\\" );
////    Model->SetColumnView( ColumnView );
////    connect( Model, &FSModel::sigChangedDirectory, this, &CmpPanel::oo_ChangedDirectory );
////
////    ProxyModel->setSourceModel( Model );
////
////    View->removeBands();
////    View->addBand( tr( "FS" ) );
////
////    // TODO: CColumnMgr 은 당연히 전역 싱글턴으로 분리되어야 한다. 
////    View->setModel( ProxyModel );
////    
////    BaseOpts.setBackgroundColor( "black" );
////    
////    // 모든 단축키 등 해제
////    for( const auto& act : View->actions() )
////    {
////        (void)act->disconnect();
////        act->setShortcut( {} );
////    }
////    
////    Qtitan::GridBandedTableColumn* GridColumn = nullptr;
////    
////    // GridColumn = View->addColumn( 0, )
////    
////    for( const auto& Column : ColumnView.VecColumns )
////    {
////        GridColumn = View->addColumn( Column.Index, Column.Name );
////        if( GridColumn != nullptr )
////        {
////            GridColumn->setBandIndex( 0 );
////            GridColumn->setAutoWidth( true );
////            GridColumn->setRowIndex( Column.Row );
////            GridColumn->setMinWidth( 30 );
////            // GridColumn->setWidth( 100 );
////            GridColumn->setTextAlignment( Column.Align );
////            GridColumn->setTextColor( "silver" );
////            GridColumn->setDecorationColor( "blue" );   // 컬럼 배경 색
////            GridColumn->editorRepository()->setEditorActivationPolicy( GridEditor::ActivationPolicy() );
////            if( Column.Content.contains( "HC.Fs.name", Qt::CaseInsensitive ) == true )
////            {
////                GridColumn->setSortLocaleAware( true );
////                GridColumn->setSortOrder( Qtitan::SortAscending, true );
////                GridColumn->dataBinding()->setSortRole( Qt::UserRole );
////                
////            }
////            GridColumn->setFilterButtonVisible( false );
////        }
////    }
////    
////    connect( View, &Qtitan::GridViewBase::contextMenu, this, &CmpPanel::oo_grdLocal_contextMenu );
////    connect( View, &Qtitan::GridViewBase::cellClicked, this, &CmpPanel::oo_grdLocal_cellClicked );
////    connect( View, &Qtitan::GridViewBase::rowDblClicked, this, &CmpPanel::oo_grdLocal_rowDblClicked );
////    connect( View, &Qtitan::GridViewBase::focusRowChanged, this, &CmpPanel::oo_grdLocal_rowFocusChanged );
////    connect( View, &Qtitan::GridViewBase::editorStarted, this, &CmpPanel::oo_grdLocal_editorStarted );
////    connect( View, &Qtitan::GridViewBase::editorPosting, this, &CmpPanel::oo_grdLocal_editorPosting );
////    connect( View, &Qtitan::GridViewBase::selectionChanged, this, &CmpPanel::oo_grdLocal_selectionChanged );
////
////    View->endUpdate();
////    
////    auto State = std::make_shared<TyTabState>( currentIndex, View, Model, ProxyModel );
////    vecTabStates.push_back( State );
////
////    Model->ChangeDirectory( QModelIndex() );
////    View->bestFit( FitToHeaderPercent );
////    
////    return currentIndex;
////}
////
//

////QModelIndex CmpPanel::retrieveFocusViewCursorIndex() const
////{
////    const auto View = retrieveFocusView();
////    if( View == nullptr )
////        return {};
////
////    const auto& Row = View->focusedRow();
////    if( Row.isValid() == false )
////        return {};
////
////    return Row.modelIndex( 0 );
////}
