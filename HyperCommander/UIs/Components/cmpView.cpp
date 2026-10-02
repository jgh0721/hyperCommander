#include "stdafx.h"
#include "cmpView.hpp"
#include "UIs/dlgOpts.hpp"
#include "UniqueLibs/qtWorkspaceState.hpp"

#include "builtInFsModel.hpp"
#include "cmnHCUtils.hpp"
#include "commandMgr.hpp"
#include "qtitanColumnAdapter.hpp"
#include "qtViewerHost.hpp"
#include "Modules/mdlFileModel.hpp"

#include <QtitanGrid.h>

#include "QtitanMSVSDebug.h"

DECLARE_CMNLIBSV2_NAMESPACE

//////////////////////////////////////////////////////////////////////////////
///

CViewT::CViewT( QWidget* Parent, Qt::WindowFlags f )
    : QWidget( Parent, f )
{
}

void CViewT::ApplyColorScheme( const TyColorScheme& ColorScheme )
{
    
}

bool CViewT::eventFilter( QObject* Watched, QEvent* Event )
{
    if( Event->type() == QEvent::FocusIn )
    {
        emit sig_NotifyViewActivated();

        //const auto Grid = qobject_cast< Qtitan::Grid* >( Watched );
        //if( Grid != nullptr && dynamic_cast< QFocusEvent* >( Event )->gotFocus() == true )
        //    emit sig_NotifyPanelActivated();
    }

    return QWidget::eventFilter( Watched, Event );
}

//////////////////////////////////////////////////////////////////////////////
///

CGridView::CGridView( QWidget* Parent, Qt::WindowFlags f )
    : CViewT( Parent, f )
{
    ( void )winId();
    setAttribute( Qt::WA_KeyCompression, true );
    setAttribute( Qt::WA_InputMethodEnabled, true );

    viewMode_ = VM_GRID;

    vbox_ = new QVBoxLayout;
    grid_ = new Qtitan::Grid;
    grid_->setProperty("hc.fileGrid", true);

    grid_->setViewType( Qtitan::Grid::BandedTableView );
    grid_->setSizePolicy( QSizePolicy( QSizePolicy::Policy::Expanding, QSizePolicy::Policy::Expanding ) );
    grid_->setAttribute( Qt::WA_KeyCompression, true );
    grid_->setAttribute( Qt::WA_InputMethodEnabled, true );
    grid_->viewport()->setAttribute( Qt::WA_KeyCompression, true );
    grid_->viewport()->setAttribute( Qt::WA_InputMethodEnabled, true );
    grid_->viewport()->setFocusPolicy( Qt::StrongFocus );

    grid_->installEventFilter( this );

    grid_->setContextMenuPolicy( Qt::CustomContextMenu );

    view_ = grid_->view< GridBandedTableView >();

    view_->beginUpdate();
    auto& BaseOpts = view_->options();
    auto& TableOpts = view_->tableOptions();
    auto& BandedOpts = view_->bandedOptions();

    BaseOpts.setShowWaitCursor( true );
    BaseOpts.setGroupsHeader( false );
    BaseOpts.setScrollBarsTransparent( true );
    BaseOpts.setMainMenuDisabled( false );              // true 이면 
    BaseOpts.setAutoCreateColumns( false );
    BaseOpts.setColumnMovingEnabled( false );
    BaseOpts.setAlternatingRowColors( false );
    BaseOpts.setScrollRowStyle( Qtitan::ScrollByPixel );
    BaseOpts.setGridLines( Qtitan::LinesNone );
    BaseOpts.setModelDecoration( true );
    BaseOpts.setModelItemsDragEnabled( true );
    BaseOpts.setRowStyle( Qtitan::GridViewOptions::RowStyleNormal );
    BaseOpts.setSelectedDecorationOpacity( 0.2 );
    BaseOpts.setSelectionPolicy( Qtitan::GridViewOptions::IgnoreSelection );
    BaseOpts.setFieldChooserEnabled( false );
    BaseOpts.setRubberBandSelection( false );
    BaseOpts.setTransparentBackground( true );
    BaseOpts.setBackgroundColor( QColor( 0, 0, 0 ) );
    BaseOpts.setSelectedBackgroundColor( QColor( 255, 112, 43 ) );
    BaseOpts.setSelectedInverseColor( true );
    BaseOpts.setModelDecorationOpacityRole( CFsModelT::USR_ROLE_HIDDENOPACITY );
    BaseOpts.setRowAutoHeight( true );

    BaseOpts.setGroupsHeaderText( "Fdsfds" );
    BaseOpts.setColumnHeaderHints( true );
    
    BaseOpts.setFocusFollowsMouse( false );
    BaseOpts.setFocusFrameEnabled( false );
    BaseOpts.setShowFocusDecoration( true );
    BaseOpts.setKeepFocusedRow( true );
    view_->setFileManagerMode(true);
    BaseOpts.setRowFocusColor( QColor( 51, 79, 102 ) );
    
    BaseOpts.setDragEnabled( true );
    BaseOpts.setDropEnabled( true );
    BaseOpts.setFieldChooserEnabled( false );
    BaseOpts.setGroupsHeaderTextColor( "white" );
    
    TableOpts.setRowsQuickSelection( false );
    TableOpts.setRowFrozenButtonVisible( false );
    TableOpts.setColumnsQuickCustomization( false );
    
    BandedOpts.setBandsHeader( false );
    BandedOpts.setBandsQuickCustomization( false );

    view_->removeBands();
    view_->addBand( tr( "FS" ) );

    // 모든 단축키 등 해제
    for( const auto& act : view_->actions() )
    {
        ( void )act->disconnect();
        act->setShortcut( {} );
    }

    connect( view_, &Qtitan::GridViewBase::contextMenu,         this, &CGridView::oo_grid_contextMenu );
    connect( view_, &Qtitan::GridViewBase::cellClicked,         this, &CGridView::oo_grid_cellClicked );
    connect( view_, &Qtitan::GridViewBase::rowDblClicked,       this, &CGridView::oo_grid_rowDblClicked );
    connect( view_, &Qtitan::GridViewBase::focusRowChanged,     this, &CGridView::oo_grid_rowFocusChanged );
    connect( view_, &Qtitan::GridViewBase::editorStarted,       this, &CGridView::oo_grid_editorStarted );
    connect( view_, &Qtitan::GridViewBase::editorPosting,       this, &CGridView::oo_grid_editorPosting );
    connect( view_, &Qtitan::GridViewBase::selectionChanged,    this, &CGridView::oo_grid_selectionChanged );

    view_->endUpdate();

    vbox_->setContentsMargins( 0, 9, 0, 0 );
    auto* QueryBar = new QHBoxLayout;
    columnStatus_ = new QLabel(this); columnStatus_->setObjectName("hc.columns.queryStatus"); columnStatus_->setWordWrap(true);
    auto* Stop = new QPushButton(tr("컬럼 조회 중지"), this); Stop->setObjectName("hc.columns.stop");
    auto* Resume = new QPushButton(tr("다시 조회"), this); Resume->setObjectName("hc.columns.resume");
    QueryBar->addWidget(columnStatus_, 1); QueryBar->addWidget(Stop); QueryBar->addWidget(Resume);
    auto* QueryWidget = new QWidget(this); QueryWidget->setLayout(QueryBar); QueryWidget->hide();
    vbox_->addWidget(QueryWidget); vbox_->addWidget( grid_ );
    pageBar_ = new QWidget(this); pageBar_->setObjectName("hc.entries.pageBar");
    auto* PageLayout = new QHBoxLayout(pageBar_);
    auto* PreviousPage = new QPushButton(tr("이전 페이지"), pageBar_); PreviousPage->setObjectName("hc.entries.previous");
    auto* NextPage = new QPushButton(tr("다음 페이지"), pageBar_); NextPage->setObjectName("hc.entries.next");
    auto* PageLabel = new QLabel(pageBar_); PageLabel->setObjectName("hc.entries.pageStatus");
    PageLayout->addWidget(PreviousPage); PageLayout->addWidget(PageLabel, 1); PageLayout->addWidget(NextPage);
    pageBar_->hide(); vbox_->addWidget(pageBar_);
    connect(PreviousPage, &QPushButton::clicked, this, [this] {
        if (auto* Model = qobject_cast<CFSModel*>(model_)) Model->SetPage(Model->PageOffset() >= CFSModel::PageSize ? Model->PageOffset() - CFSModel::PageSize : 0);
    });
    connect(NextPage, &QPushButton::clicked, this, [this] {
        if (auto* Model = qobject_cast<CFSModel*>(model_)) Model->SetPage(Model->PageOffset() + CFSModel::PageSize);
    });
    connect(Stop, &QPushButton::clicked, this, [this] { if (auto* Model = qobject_cast<CFSModel*>(model_)) Model->CancelColumnQueries(); });
    connect(Resume, &QPushButton::clicked, this, [this] { if (auto* Model = qobject_cast<CFSModel*>(model_)) Model->ResumeColumnQueries(); });
    columnSortTimer_.setSingleShot(true);
    connect(view_, &Qtitan::GridViewBase::sortingChanged, this, [this] { columnSortTimer_.start(0); });
    connect(&columnSortTimer_, &QTimer::timeout, this, [this] {
        if (auto* Model = qobject_cast<CFSModel*>(model_)) {
            const auto Selected = CaptureSelection(); const auto Focus = CaptureFocus();
            Model->SetColumnSorts(ColumnProfile().sorts_); RestoreSelection(Selected, Focus);
        }
    });
    setLayout( vbox_ );
    setSizePolicy( QSizePolicy( QSizePolicy::Policy::Expanding, QSizePolicy::Policy::Expanding ) );

    // Percentage/fixed widths are applied by QtitanColumnAdapter after header measurement.
    view_->bestFit( FitToHeader );
    columnAdapter_ = new QtitanColumnAdapter(this, grid_, view_);
}

void CGridView::SetModel( CFsModelT* Source, QAbstractProxyModel* Proxy, ColumnView* Columns )
{
    if (Source && !Source->QObject::parent()) Source->setParent(this);
    if (Proxy && !Proxy->QObject::parent()) Proxy->setParent(this);
    if( view_ == nullptr )
        return;

    if( Source == nullptr || Columns == nullptr )
        return;

    // Qtitan's progress estimate evaluates log(0) when sorting an empty model.
    // Cancel only that vacuous sort through its public hook; retain the sort rule
    // so later asynchronous rows are sorted normally, including empty directories.
    const auto Controller = view_->modelController();
    if (!Controller->property("hc.emptySortGuard").toBool())
    {
        connect(Controller, &Qtitan::GridModelController::aboutToBeSorted, this, [Controller](bool& Cancel) {
            if (Controller->getRecordCount() == 0) Cancel = true;
        }, Qt::DirectConnection);
        Controller->setProperty("hc.emptySortGuard", true);
    }

    model_ = Source;
    proxy_ = Proxy;
    // HC-QTN-005: 8.2.0 can retain selection ranges across a model reset.
    // Clear them before rows disappear: selectedRowIndexes otherwise dereferences
    // an invalid GridRow, and old row positions must not target different files.
    disconnect(selectionReset_);
    selectionReset_ = connect(Source, &QAbstractItemModel::modelAboutToBeReset,
        this, [this] {
            resetSelection_ = CaptureSelection(); resetFocus_ = CaptureFocus();
            resetPath_ = model_->property("hc.snapshotLocation").toString();
            view_->deselectAll();
        }, Qt::DirectConnection);
    disconnect(selectionRestore_);
    selectionRestore_ = connect(Source, &CFsModelT::sigRefreshCompleted, this, [this] {
        if (!resetPath_.isEmpty() && resetPath_ == model_->property("hc.snapshotLocation").toString())
            RestoreSelection(resetSelection_, resetFocus_);
        resetPath_.clear(); resetSelection_.clear(); resetFocus_.clear();
        const auto Previous = model_->property("hc.parentFocusName").toString();
        const bool ParentNavigation = model_->property("hc.parentNavigation").toBool();
        model_->setProperty("hc.parentFocusName", QVariant{}); model_->setProperty("hc.parentNavigation", false);
        const auto Native = qobject_cast<CFSModel*>(model_);
        if (!ParentNavigation || (Native && Native->LastNavigationError())) return;
        if (view_->getRowCount()) view_->setFocusedRowIndex(0);
        if (!hc::settings::AppSettings().Read(hc::settings::Section::General).toObject()["focusPreviousDirectory"].toBool(true)) return;
        for (int Index = 0; !Previous.isEmpty() && Index < model_->rowCount(); ++Index) {
            const auto Source = model_->index(Index, 0);
            if (model_->GetName(Source) != Previous) continue;
            const auto Visible = proxy_ ? proxy_->mapFromSource(Source) : Source;
            if (!Visible.isValid()) break;
            const auto Row = view_->getRow(Visible);
            if (Row.isValid()) view_->setFocusedRowIndex(Row.rowIndex());
            break;
        }
    });
    if (auto* Native = qobject_cast<CFSModel*>(model_); Native && !Native->property("hc.columnSignals").toBool()) {
        Native->setProperty("hc.columnSignals", true);
        connect(Native, &CFSModel::PageChanged, this, [this, Native] {
            pageBar_->setVisible(Native->IsPaged());
            pageBar_->findChild<QLabel*>("hc.entries.pageStatus")->setText(tr("%1–%2 / %3개 · 선택은 현재 페이지에 적용")
                .arg(Native->EntryCount() ? Native->PageOffset() + 1 : 0)
                .arg(std::min(Native->PageOffset() + CFSModel::PageSize, Native->EntryCount())).arg(Native->EntryCount()));
            pageBar_->findChild<QPushButton*>("hc.entries.previous")->setEnabled(Native->PageOffset() != 0);
            pageBar_->findChild<QPushButton*>("hc.entries.next")->setEnabled(Native->PageOffset() + CFSModel::PageSize < Native->EntryCount());
        });
        connect(Native, &CFSModel::ColumnValuesChanging, this, [this] { querySelection_ = CaptureSelection(); queryFocus_ = CaptureFocus(); });
        connect(Native, &CFSModel::ColumnValuesChanged, this, [this] { RestoreSelection(querySelection_, queryFocus_); });
        connect(Native, &CFSModel::ColumnQueryStatus, this, [this](const QString& Text) { columnStatus_->setText(Text); columnStatus_->parentWidget()->setVisible(!Text.isEmpty()); });
        connect(Native, &CFSModel::ColumnSortCancelled, this, [this, Native] {
            view_->beginUpdate();
            for (int Index = 0; Index < model_->columnCount(); ++Index) static_cast<Qtitan::GridTableColumn*>(view_->getColumnByModelColumn(Index))->setSortOrder(Qtitan::SortNone);
            for (const auto& Sort : Native->ColumnSorts()) for (int Index = 0; Index < model_->columnCount(); ++Index)
                if (QUuid(columnAdapter_->ColumnId(Index)) == Sort.columnId_) static_cast<Qtitan::GridTableColumn*>(view_->getColumnByModelColumn(Index))->setSortOrder(Sort.order_ == Qt::AscendingOrder ? Qtitan::SortAscending : Qtitan::SortDescending);
            view_->endUpdate();
            columnSortTimer_.stop();
        });
    }
    model_->SetColumnView( *Columns );

    if( Proxy != nullptr )
    {
        proxy_->setSourceModel( model_ );
        view_->setModel( proxy_ );
    }
    else
    {
        view_->setModel( model_ );
    }

    view_->beginUpdate();
    Qtitan::GridBandedTableColumn* GridColumn = nullptr;
    
    for( const auto& Column : Columns->VecColumns )
    {
        GridColumn = view_->addColumn( Column.Index, Column.definition_.title_ );
        if( GridColumn != nullptr )
        {
            GridColumn->setBandIndex( 0 );
            GridColumn->setAutoWidth( true );
            GridColumn->setRowIndex( 0 );
            GridColumn->setMinWidth( 30 );
            // GridColumn->setWidth( 100 );
            GridColumn->setTextAlignment( Column.definition_.alignment_ );
            GridColumn->editorRepository()->setEditorActivationPolicy( GridEditor::ActivationPolicy() );
            GridColumn->dataBinding()->setSortRole(static_cast<Qt::ItemDataRole>(CFSModel::GridSortRole));
            if (Column.definition_.id_ == Columns->profile_.primaryNameId_)
            {
                GridColumn->setSortLocaleAware( true );
                
            }
            GridColumn->setFilterButtonVisible( false );
        }
    }

    // Percentage/fixed widths are applied by QtitanColumnAdapter after header measurement.
    view_->bestFit( FitToHeader );
    view_->endUpdate();
    for (int Position = 0; Position < Columns->profile_.columns_.size(); ++Position)
        for (const auto& Column : Columns->VecColumns)
            if (Column.definition_.id_ == Columns->profile_.columns_[Position].id_)
                view_->getColumnByModelColumn(Column.Index)->setVisualIndex(Position);
    for (const auto& Sort : Columns->profile_.sorts_) {
        for (const auto& Column : Columns->VecColumns)
            if (Column.definition_.id_ == Sort.columnId_)
                static_cast<Qtitan::GridTableColumn*>(view_->getColumnByModelColumn(Column.Index))->setSortOrder(
                    Sort.order_ == Qt::AscendingOrder ? Qtitan::SortAscending : Qtitan::SortDescending);
    }
    columnAdapter_->Bind(*Columns);
    if (auto* Native = qobject_cast<CFSModel*>(model_)) Native->SetColumnSorts(Columns->profile_.sorts_);
    ApplyDisplaySettings();
}

void CGridView::ApplyDisplaySettings()
{
    if (!model_ || !view_) return;
    const auto General = hc::settings::AppSettings().Read(hc::settings::Section::General).toObject();
    const auto Selection = CaptureSelection(); const auto Focus = CaptureFocus();
    view_->beginUpdate();
    if (auto* Model = qobject_cast<CFSModel*>(model_)) Model->ApplyDisplaySettings();
    for (int Index = 0; Index < model_->columnCount(); ++Index) {
        auto* Column = static_cast<Qtitan::GridTableColumn*>(view_->getColumnByModelColumn(Index)); if (!Column) continue;
        Column->setSortCaseSensitivity(General["sortCaseSensitive"].toBool());
        Column->setSortLocaleAware(General["sortLocaleAware"].toBool(true));
        const auto Order = Column->sortOrder();
        Column->setSortOrder(Order, General["sortNumericAware"].toBool(true));
    }
    view_->modelController()->refresh();
    view_->endUpdate();
    RestoreSelection(Selection, Focus);
}

void CGridView::ApplyColorScheme( const TyColorScheme& ColorScheme )
{
    if( ColorScheme.Name.isEmpty() == true )
        return;

    if( view_ == nullptr )
        return;

    if( model_ != nullptr )
        model_->SetColorScheme( ColorScheme );

    ApplyDisplaySettings();
    // RowFocus 색상 등 적용
    view_->beginUpdate();
    auto& BaseOpts = view_->options();
    grid_->setFont(ColorScheme.FileList_Font);
    BaseOpts.setTransparentBackground(false);
    view_->fileManagerOptions().setInvertedCursor(ColorScheme.IsInverseCursor);
    BaseOpts.setSelectedInverseColor(ColorScheme.IsInverseSelect);
    // Qtitan pins the viewport palette and caches header primitives. A theme
    // change must refresh both even when the QApplication style is unchanged.
    grid_->setPalette(QApplication::palette());
    grid_->viewport()->setPalette(QApplication::palette());
    QEvent StyleChange(QEvent::StyleChange);
    QApplication::sendEvent(grid_, &StyleChange);
    for (int Index = 0; model_ && Index < model_->columnCount(); ++Index)
        if (auto* Column = view_->getColumnByModelColumn(Index)) {
            Column->setTextColor(ColorScheme.FileList_FGColor);
            Column->setBackgroundColor(Qt::transparent);
        }

    if( ColorScheme.FileList_BGColor.isValid() == true )
        BaseOpts.setBackgroundColor( ColorScheme.FileList_BGColor );

    if( ColorScheme.FileList_Selected.isValid() == true )
        BaseOpts.setSelectedBackgroundColor( ColorScheme.FileList_Selected );

    if( ColorScheme.FileList_Cursor.isValid() == true )
        BaseOpts.setRowFocusColor( ColorScheme.FileList_Cursor );

    view_->endUpdate();
}

void CGridView::SetPanelActive(bool Active)
{
    // Keep the logical cursor and marks intact; only suppress cursor painting.
    if (view_ && view_->options().showFocusDecoration() != Active)
        view_->options().setShowFocusDecoration(Active);
}

bool CGridView::IsEditing() const
{
    if( view_ == nullptr )
        return false;

    return view_->activeEditor() != nullptr;
}

void CGridView::AdjustSizeToContents() const
{
    if (columnAdapter_) columnAdapter_->Request();

    CViewT::AdjustSizeToContents();
}

void CGridView::ConfigureColumns()
{
    QMainOpts Dialog(window()); Dialog.SetListView(this); Dialog.ShowListSettingsPage(); Dialog.exec();
}

hc::columns::ColumnProfile CGridView::ColumnProfile() const
{
    auto Profile = columnAdapter_->Profile();
    QMap<QUuid, int> Positions;
    for (int Index = 0; Index < model_->columnCount(); ++Index)
        if (const auto* Column = view_->getColumnByModelColumn(Index)) Positions[QUuid(columnAdapter_->ColumnId(Index))] = Column->visualIndex();
    std::stable_sort(Profile.columns_.begin(), Profile.columns_.end(), [&](const auto& Left, const auto& Right) {
        return Positions.value(Left.id_, -1) < Positions.value(Right.id_, -1);
    });
    Profile.sorts_.clear();
    const auto Rules = view_->modelController()->sortRules();
    for (int Rule = 0; Rule < Rules->sortRulesCount(); ++Rule)
        for (int Index = 0; Index < model_->columnCount(); ++Index) {
            auto* Column = static_cast<Qtitan::GridTableColumn*>(view_->getColumnByModelColumn(Index));
            if (Column && Rules->sortRule(Rule)->dataBinding == Column->dataBinding())
                Profile.sorts_.append({QUuid(columnAdapter_->ColumnId(Index)), Column->sortOrder() == Qtitan::SortDescending ? Qt::DescendingOrder : Qt::AscendingOrder});
        }
    return Profile;
}

bool CGridView::ApplyColumnProfile(const hc::columns::ColumnProfile& Profile)
{
    if (!model_ || !hc::columns::IsSupportedProfile(Profile)) return false;
    const auto Selected = CaptureSelection(); const auto Focus = CaptureFocus();
    view_->beginUpdate();
    view_->deselectAll(); view_->setModel(nullptr); view_->removeColumns();
    auto Binding = BindColumnProfile(Profile);
    SetModel(model_, proxy_, &Binding);
    columnAdapter_->UseProfileWidths();
    columnAdapter_->Apply();
    view_->endUpdate();
    RestoreSelection(Selected, Focus);
    return true;
}

QJsonObject CGridView::CaptureWorkspaceView() const
{
    QJsonArray Columns;
    for (int Index = 0; Index < model_->columnCount(); ++Index)
        if (auto* Column = static_cast<Qtitan::GridTableColumn*>(view_->getColumnByModelColumn(Index)))
        {
            int Priority = -1;
            const auto Rules = view_->modelController()->sortRules();
            for (int Rule = 0; Rule < Rules->sortRulesCount(); ++Rule)
                if (Rules->sortRule(Rule)->dataBinding == Column->dataBinding()) { Priority = Rule; break; }
            Columns.append(QJsonObject{{"id", columnAdapter_->ColumnId(Index)}, {"position", Column->visualIndex()},
                {"sort", int(Column->sortOrder())}, {"priority", Priority}});
        }
    const auto Proxy = qobject_cast<CFSProxyModel*>(proxy_);
    auto Layout = columnAdapter_->WorkspaceLayout();
    if (columnAdapter_->Writable()) Layout["definition"] = hc::columns::EncodeProfile(ColumnProfile());
    return {{"mode", "details"}, {hc::columns::WorkspaceKey, Layout}, {"columnOrderV2", Columns},
        {"hiddenSystem", Proxy && Proxy->GetHiddenSystem()}, {"search", Proxy ? Proxy->Search() : QString{}}};
}

void CGridView::RestoreWorkspaceView(const QJsonObject& View)
{
    const auto Layout = View.value(hc::columns::WorkspaceKey).toObject();
    auto Definition = hc::columns::DecodeProfile(Layout.value("definition").toObject());
    if (Layout.isEmpty()) {
        const auto Defaults = hc::settings::AppSettings().Read(hc::settings::Section::Columns).toObject()
            ["ColumnsV2/Defaults/" + columnAdapter_->PaneKey()].toObject();
        Definition = hc::columns::DecodeProfile(Defaults);
    }
    if (Definition && hc::columns::IsSupportedProfile(*Definition)) {
        ApplyColumnProfile(*Definition);
        if (!Layout.isEmpty()) columnAdapter_->RestoreWorkspaceLayout(View);
    } else columnAdapter_->RestoreWorkspaceLayout(View);
    QList<QJsonObject> Sorted;
    for (const auto& Value : View.value("columnOrderV2").toArray()) Sorted.append(Value.toObject());
    std::stable_sort(Sorted.begin(), Sorted.end(), [](const auto& A, const auto& B) {
        return A.value("priority").toInt(-1) < B.value("priority").toInt(-1);
    });
    view_->beginUpdate();
    if (!Sorted.isEmpty())
        for (int Index = 0; Index < model_->columnCount(); ++Index)
            if (auto* Column = static_cast<Qtitan::GridTableColumn*>(view_->getColumnByModelColumn(Index))) Column->setSortOrder(Qtitan::SortNone);
    for (const auto& Value : Sorted)
        for (int Index = 0; Index < model_->columnCount(); ++Index)
        {
            if (columnAdapter_->ColumnId(Index) != Value.value("id").toString()) continue;
            auto* Column = static_cast<Qtitan::GridTableColumn*>(view_->getColumnByModelColumn(Index));
            if (!Column) continue;
            const auto Position = Value.value("position").toInt(-1);
            if (Position >= 0 && Position < model_->columnCount()) Column->setVisualIndex(Position);
            const auto Sort = Value.value("sort").toInt(-1);
            if (Sort == Qtitan::SortAscending || Sort == Qtitan::SortDescending)
                Column->setSortOrder(static_cast<Qtitan::SortOrder>(Sort), hc::settings::AppSettings().Read(hc::settings::Section::General).toObject()["sortNumericAware"].toBool(true));
        }
    view_->endUpdate();
    if (auto* Proxy = qobject_cast<CFSProxyModel*>(proxy_))
    { Proxy->SetHiddenSystem(View.value("hiddenSystem").toBool()); Proxy->SetSearch(View.value("search").toString()); }
}

QStringList CGridView::CaptureSelection() const
{
    QStringList Result;
    if (!model_ || !view_) return Result;
    for (const auto Row : view_->selection()->selectedRowIndexes())
    {
        auto Index = view_->getRow(Row).modelIndex(0);
        if (!Index.isValid()) continue;
        if (proxy_) Index = proxy_->mapToSource(Index);
        const auto Item = model_->GetFileInfo(Index);
        if (Item && Item->storageEntry_) Result.append(hc::workspace::SelectionKey(*Item->storageEntry_));
    }
    return Result;
}

QString CGridView::CaptureFocus() const
{
    if (!model_ || !view_) return {};
    auto Index = view_->focusedRow().modelIndex(0);
    if (!Index.isValid()) return {};
    if (proxy_) Index = proxy_->mapToSource(Index);
    const auto Item = model_->GetFileInfo(Index);
    return Item && Item->storageEntry_ ? hc::workspace::SelectionKey(*Item->storageEntry_) : QString{};
}

void CGridView::RestoreSelection(const QStringList& Selection, const QString& Focus)
{
    const QSet<QString> Keys(Selection.begin(), Selection.end());
    view_->deselectAll();
    for (int Index = 0; Index < model_->rowCount(); ++Index)
    {
        const auto Source = model_->index(Index, 0);
        const auto Item = model_->GetFileInfo(Source);
        if (!Item || !Item->storageEntry_) continue;
        const auto Key = hc::workspace::SelectionKey(*Item->storageEntry_);
        if (!Keys.contains(Key) && Key != Focus) continue;
        const auto Visible = proxy_ ? proxy_->mapFromSource(Source) : Source;
        if (!Visible.isValid()) continue;
        const auto Row = view_->getRow(Visible);
        if (!Row.isValid()) continue;
        if (Keys.contains(Key)) view_->selectRow(Row.rowIndex(), Qtitan::Select);
        if (Key == Focus) view_->setFocusedRowIndex(Row.rowIndex());
    }
}

void CGridView::EnsureKeyboardFocusOnView() const
{
    if( view_ == nullptr )
        return;

    grid_->setFocus( Qt::MouseFocusReason );
    grid_->viewport()->setFocus( Qt::MouseFocusReason );
    grid_->viewport()->raise();
    grid_->viewport()->activateWindow();

    if( view_->getRowCount() > 0 )
    {
        if( view_->focusedRowIndex() == view_->modelController()->getGridRowCount() - 1 )
        {
            // 가장 마지막임
            view_->navigateUp();
            view_->navigateDown();
        }
        else
        {
            view_->navigateDown();
            view_->navigateUp();
        }
    }

    CViewT::EnsureKeyboardFocusOnView();
}

nsHC::TySpFileSource CGridView::SelectRowOnCurrentTab( const QModelIndex& SrcIndex, bool IsMoveDown, bool& IsSelected )
{
    // TODO: 각 패널의 하단에 선택된 항목 수 및 크기 등에 대해 갱신해야 한다.
    // State 에 저장된 항목들 갱신
    // TODO: 선택된 항목이 디렉토리이고, EverythingSDK 를 쓰지 않고, 디렉토리 계산 기능이 사용 중이라면 디렉토리 크기를 계산한다. 

    if( view_ == nullptr )
        return {};
    
    const auto Row = view_->getRow( SrcIndex );
    const auto ModelIndex = proxy_ != nullptr ? proxy_->mapToSource( SrcIndex ) : SrcIndex;
    nsHC::TySpFileSource Info = model_->GetFileInfo( ModelIndex );

    view_->selectRow( Row.rowIndex(), Qtitan::Invert );
    IsSelected = view_->modelController()->isRowSelected( Row );
    
    if( IsMoveDown == true )
        view_->navigateDown( Qt::NoModifier );

    return Info;
}

void CGridView::ContextMenuOnCurrentTab( const QModelIndex& SrcIndex )
{
    QRect Rect;
    PopupMenu menu( grid_ );
    const auto Row = view_->getRow( SrcIndex );

    for( const auto Info : grid_->hitInfoAll() )
    {
        if( Info.info() != GridHitInfo::Cell )
            continue;

        if( Info.row().rowIndex() == Row.rowIndex() )
        {
            Rect = Info.rect();
            break;
        }
    }
    
    GridHitInfo HitInfo( GridHitInfo::Cell, view_, Rect, Row.rowIndex(), Row.cell( 0 ).columnIndex() );
    ContextMenuEventArgs Args( view_, &menu, HitInfo );
    oo_grid_contextMenu( &Args );
}

Qtitan::GridViewBase* CGridView::BaseView() const
{
    return view_;
}

CFsModelT* CGridView::BaseModel() const
{
    return model_;
}

//////////////////////////////////////////////////////////////////////////////
///

void CGridView::oo_grid_contextMenu( Qtitan::ContextMenuEventArgs* Args )
{
    const auto Info = Args->hitInfo();
    const auto Type = Info.info();
    const auto ModelIndex = proxy_ != nullptr ? proxy_->mapToSource( Info.modelIndex() ) : Info.modelIndex();

    viewClickTimer_.stop();

    switch( Type )
    {
        case GridHitInfo::Cell:
        {

            // NOTE: Qt 5 에서 서로 다른 해상도를 가진 다중 모니터 환경에서 정확한 커서 위치를 얻어올 수 없음.
            // 따라서, Win32 API 인 GetCursorPos 로 대체한다. 
            POINT Pos = {};
            GetCursorPos( &Pos );

            OpenShellContextMenuForObject( ( ( CFSModel* )model_ )->GetFileFullPath( ModelIndex ).toStdWString(), Pos.x, Pos.y,
                                           reinterpret_cast< HWND >( Args->view()->grid()->winId() ) );
        } break;
        case GridHitInfo::Rows:
        {
            POINT Pos = {};
            GetCursorPos( &Pos );

            OpenShellContextMenuForObject( model_->GetCurrentPathWithRoot().toStdWString(), Pos.x, Pos.y,
                                           reinterpret_cast< HWND >( Args->view()->grid()->winId() ) );
        } break;
    }

    Args->setHandled( true );
}

void CGridView::oo_grid_cellClicked( Qtitan::CellClickEventArgs* Args )
{
    const auto& Cell = Args->cell();

    //// 자신을 포함한 여러 행을 선택하고, 그 중에 하나를 다시 선택하면 해당 행만 남기고 나머지는 선택 해제한다.
    //// 선택 행이 하나이고, 해당 행을 다시 클릭했을 때는 이름 변경 시도를 고려해야 한다. 
    //if( spLocalView_->modelController()->isRowSelected( cell.row() ) == true )
    //{
    //    if( spLocalView_->modelController()->selection()->selectedRowIndexes().count() > 1 )
    //    {
    //        if( QGuiApplication::queryKeyboardModifiers() == Qt::NoModifier )
    //        {
    //            spLocalView_->deselectAll();
    //            spLocalView_->selectRow( cell.row().rowIndex() );
    //            return;
    //        }
    //    }
    //}

    ////if( cell.columnIndex() != TBL_LOCAL_HDX_FILENAME.index )
    ////{
    ////    lastFocusRowIndex_ = spLocalView_->focusedRowIndex();
    ////    viewClickTimer_.stop();
    ////    return;
    ////}

    //if( retrieveCurrentIndex() >= 0 )
    //{
    //    if( Cell.columnIndex() != 0 )
    //    {
    //        viewClickTimer.stop();
    //    }

    //    const auto Modifiers = QGuiApplication::queryKeyboardModifiers();
    //    if( Modifiers.testFlag( Qt::ControlModifier ) )
    //    {
    //        SelectRowOnCurrentTab( Args->cell().modelIndex(), false );
    //        Args->setHandled( true );
    //    }

    //    if( Cell.columnIndex() == 0 && 
    //        Modifiers == Qt::NoModifier )
    //    {
    //        const auto State = retrieveFocusState();
    //        const auto View = State->View;

    //        if( State->LastFocusedRowIndex != View->focusedRowIndex() )
    //        {
    //            State->LastFocusedRowIndex = View->focusedRowIndex();
    //            viewClickTimer.stop();
    //            return;
    //        }

    //        static const auto doubleClickInterval = qApp->styleHints()->mouseDoubleClickInterval();
    //        viewClickTimer.setSingleShot( true );
    //        viewClickTimer.start( doubleClickInterval );
    //    }
    //}
}

void CGridView::oo_grid_rowDblClicked( Qtitan::RowClickEventArgs* Args )
{
    const auto Row = Args->row();
    const auto Index = Row.modelIndex( 0 );
    const auto StCommandMgr = TyStCommandMgr::GetInstance();

    if( view_->activeEditor() != nullptr )
        view_->closeEditor();

    viewClickTimer_.stop();

    QKeyEvent Event( QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier );

    Args->setHandled( StCommandMgr->ProcessKeyPressEvent( &Event, Index ) );
}

void CGridView::oo_grid_rowFocusChanged( int OldRowIndex, int NewRowIndex )
{
    UNREFERENCED_PARAMETER( OldRowIndex );
    UNREFERENCED_PARAMETER( NewRowIndex );

    ////qDebug() << qMakePair( OldRowIndex, NewRowIndex );

    viewClickTimer_.stop();

    //for( const auto& State : vecTabStates )
    //    State->LastFocusedRowIndex = -1;
    //
    //retrieveFocusState()->LastFocusedRowIndex = OldRowIndex;
}

void CGridView::oo_grid_editorStarted( Qtitan::EditorEventArgs* Args )
{
    //if( Args->column()->index() != 0 )
    //    return;

    //const auto View = retrieveFocusView();
    //const auto Editor = View->activeEditor();
    //const auto LineEdit = qobject_cast< QLineEdit* >( Editor->getCellWidget() );

    //// 파일이름의 경우
    //LineEdit->setTextMargins( 2, 0, 2, 0 );
    //LineEdit->setObjectName( "edtFieldOnList" );
    //LineEdit->installEventFilter( this );
    //LineEdit->setStyleSheet( "margin-left: 28px;" );        // 파일 이름 옆에 나타나는 아이콘의 크기를 측정하여 지정해야한다. 

    //// 색상 설정
    //auto Pal = QGuiApplication::palette();
    //Pal.setColor( QPalette::Base, QColor( "black" ) );
    //Pal.setColor( QPalette::WindowText, QColor( "silver" ) );
    //Pal.setColor( QPalette::Text, QColor( "silver" ) );
    //LineEdit->setPalette( Pal );

    //auto Text = LineEdit->text();

    //if( Text.contains( '.' ) == true )
    //{
    //    LineEdit->setSelection( 0, static_cast< int >( Text.lastIndexOf( '.' ) ) );
    //}
    //else
    //{
    //    LineEdit->selectAll();
    //}
}

void CGridView::oo_grid_editorPosting( Qtitan::EditorEventArgs* Args )
{
    //if( Args->column()->index() != 0 )
    //    return;

    //const auto State = retrieveFocusState();
    //const auto View = State->View;
    //const auto Editor = View->activeEditor();
    //const auto LineEdit = qobject_cast< QLineEdit* >( Editor->getCellWidget() );

    //const auto Ret = State->Model->Rename( State->ProxyModel->mapToSource( Args->row().modelIndex( 0 ) ), LineEdit->text() );

    //Args->setHandled( Ret == ERROR_SUCCESS );
}

void CGridView::oo_grid_selectionChanged( Qtitan::GridSelection* NewSelection, Qtitan::GridSelection* OldSelection )
{
    UNREFERENCED_PARAMETER( NewSelection );
    UNREFERENCED_PARAMETER( OldSelection );

    viewClickTimer_.stop();

    //for( const auto& State : vecTabStates )
    //    State->LastFocusedRowIndex = -1;
}

bool CGridView::eventFilter( QObject* Watched, QEvent* Event )
{
    if( Event->type() == QEvent::KeyPress )
    {
        if( IsEditing() == true )
            return false;

        const auto KeyEvent = dynamic_cast< QKeyEvent* >( Event );
        const auto StCommandMgr = TyStCommandMgr::GetInstance();

        qDebug() << __FUNCTION__ << " : " << KeyEvent << KeyEvent->text();

        if( StCommandMgr->ProcessKeyPressEvent( KeyEvent, retrieveCursorIndex() ) == true )
            return true;
    }

    if( Event->type() == QEvent::FocusOut )
    {
        const auto Grid = qobject_cast< Qtitan::Grid* >( Watched );
        if( Grid != nullptr )
        {
            viewClickTimer_.stop();
            lastFocusedRowIndex_ = -1;
        }
    }

    return CViewT::eventFilter( Watched, Event );
}

void CGridView::resizeEvent( QResizeEvent* event )
{
    if (columnAdapter_) columnAdapter_->Request();

    CViewT::resizeEvent( event );
}

bool CGridView::nativeEvent( const QByteArray& eventType, void* message, qintptr* result )
{
    int a = 0;
    if( eventType != "windows_generic_MSG" )
        return CViewT::nativeEvent( eventType, message, result );

    MSG* msg = static_cast< MSG* >( message );
    if( msg->message == WM_CHAR )
    {
        qDebug() << __FUNCTION__ << " : " << QChar( ushort( msg->wParam ) );
    }

    return false;
}

QModelIndex CGridView::retrieveCursorIndex() const
{
    if( view_ == nullptr )
        return {};

    const auto& Row = view_->focusedRow();
    if( Row.isValid() == false )
        return {};

    return Row.modelIndex( 0 );
}

//////////////////////////////////////////////////////////////////////////////
///

constexpr auto WLX_PROC_ATOM = L"HyperCommander";

CInternalViewer::CInternalViewer( QWidget* Parent, Qt::WindowFlags f )
    : CViewBase( Parent, f )
{
}

CExternalViewer::CExternalViewer( QWidget* Parent, Qt::WindowFlags f )
    : CViewBase( Parent, f )
{
    // NOTE: 삭제 금지
    ( void )winId();
    atom = GlobalAddAtomW( WLX_PROC_ATOM );

    vbox_ = new QVBoxLayout;
    setLayout( vbox_ );
    vbox_->setContentsMargins( 0, 0, 0, 0 );
}

CExternalViewer::~CExternalViewer()
{
    if( viewer_ == nullptr )
        return;

    const auto Funcs = viewer_->Funcs();
    if( Funcs.ListCloseWindow != nullptr && hChild != nullptr )
        Funcs.ListCloseWindow( hChild );

    GlobalDeleteAtom( atom );
}

void CExternalViewer::SetWLX( TySpWLX Viewer )
{
    viewer_ = Viewer;
}

bool CExternalViewer::Initialize()
{
    bool IsInitialized = false;

    do
    {
        if( viewer_ == nullptr )
            break;

        setDefaultParams();

        wParent = new QWidget;
        hParent = reinterpret_cast< HWND >( wParent->winId() );

        hChild = ListLoad( hParent, GetFilePath(), 0 );
        if( hChild == nullptr )
            break;

        QWindow* Child_W = QWindow::fromWinId( reinterpret_cast< WId >( hChild ) );
        Child_W->setFlag( Qt::FramelessWindowHint );
        wChild = QWidget::createWindowContainer( Child_W, wParent, Qt::FramelessWindowHint );

        // Subclass viewer window to catch WM_COMMAND message.
        HWND Result = (HWND)SetWindowLongPtrW( hParent, GWLP_WNDPROC, ( LONG_PTR )cbWndProc );
        SetPropW( hParent, WLX_PROC_ATOM, Result );

        // Subclass plugin window to catch some hotkeys like 'n' or 'p'.
        Result = ( HWND )SetWindowLongPtrW( hChild, GWLP_WNDPROC, ( LONG_PTR )cbWndProcChild );
        SetPropW( hChild, WLX_PROC_ATOM, Result );

#if QT_VERSION_MAJOR == 5
        SetWindowLongPtrW( hChild, GWLP_HWNDPARENT, reinterpret_cast< LONG_PTR >( hParent ) );
#endif
        vbox_->addWidget( wChild );
        wChild->show();
        
        viewer_->SetWinId( hChild );

        IsInitialized = true;

    } while( false );

    return IsInitialized;
}

HWND CExternalViewer::ListLoad( HWND ParentWin, const QString& FilePath, int ShowFlags )
{
    if( viewer_ == nullptr )
        return {};

    const auto Funcs = viewer_->Funcs();
    if( Funcs.ListLoadW != nullptr )
        return Funcs.ListLoadW( hParent, const_cast< WCHAR* >( FilePath.toStdWString().c_str() ), 0 );

    if( Funcs.ListLoad != nullptr )
        Funcs.ListLoad( hParent, const_cast< CHAR* >( FilePath.toStdString().c_str() ), 0 );

    return {};
}

void CExternalViewer::setDefaultParams()
{
    const auto Funcs = viewer_->Funcs();
    if( Funcs.ListSetDefaultParams == nullptr )
        return;

    typedef struct
    {
        int size;
        DWORD PluginInterfaceVersionLow;
        DWORD PluginInterfaceVersionHi;
        char DefaultIniName[ MAX_PATH ];
    } ListDefaultParamStruct;

    ListDefaultParamStruct dps = { sizeof( ListDefaultParamStruct ), 2, 0 };

    const auto ini = QDir::toNativeSeparators( QCoreApplication::applicationDirPath() + "/lsplugins.ini" ).toLocal8Bit();
    strncpy_s( dps.DefaultIniName, ini.data(), ini.size() );

    Funcs.ListSetDefaultParams( &dps );
}

LRESULT CExternalViewer::cbWndProc( HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam )
{
    LRESULT Ret = 0;
    WNDPROC Prev = nullptr;
    if( uMsg == WM_LBUTTONDOWN )
    {
        int a = 0;
    }

    Prev = ( WNDPROC )GetPropW( hWnd, WLX_PROC_ATOM );
    if( Prev )
        Ret = CallWindowProcW( Prev, hWnd, uMsg, wParam, lParam );
    else
        Ret = DefWindowProcW( hWnd, uMsg, wParam, lParam );

    if( Ret == 0 && uMsg == WM_COMMAND && lParam != 0 )
    {
        const auto Window = QWindow::fromWinId( reinterpret_cast< WId >( hWnd ) );
        // Window->

        //    Lister:= TControl(GetLCLOwnerObject(hWnd));
        //    if Assigned(Lister) then Lister.Perform(Msg, wParam, lParam);
    }

    return Ret;

    //  if (Result = 0) and (Msg = WM_COMMAND) and (lParam <> 0) then
    //  begin
    //    Lister:= TControl(GetLCLOwnerObject(hWnd));
    //    if Assigned(Lister) then Lister.Perform(Msg, wParam, lParam);
    //  end;
    //end;
}

LRESULT CExternalViewer::cbWndProcChild( HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam )
{
    LRESULT Ret = 0;
    WNDPROC Prev = nullptr;
    if( uMsg == WM_LBUTTONDOWN )
    {
        int a = 0;
    }

    Prev = ( WNDPROC )GetPropW( hWnd, WLX_PROC_ATOM );
    if( Prev )
        Ret = CallWindowProcW( Prev, hWnd, uMsg, wParam, lParam );
    else
        Ret = DefWindowProcW( hWnd, uMsg, wParam, lParam );

    if( Ret == 0 && uMsg == WM_KEYDOWN )
        PostMessageW( GetParent( hWnd ), uMsg, wParam, lParam );

    return Ret;
}

//////////////////////////////////////////////////////////////////////////////
///

CQuickView::CQuickView( QWidget* Parent, Qt::WindowFlags f )
    : CViewT( Parent, f )
{
    vbox_ = new QVBoxLayout;
    viewMode_ = VM_QUICK;

    setLayout( vbox_ );
    vbox_->setContentsMargins( 0, 0, 0, 0 );
}

void CQuickView::SetFilePath( const QString& FilePath )
{
    filePath_ = FilePath;
}

void CQuickView::showEvent( QShowEvent* event )
{
    if (surface_ || filePath_.isEmpty()) { CViewT::showEvent(event); return; }
    do
    {
        if( event->spontaneous() == false && isInitOnce_ == false )
        {
            const auto StPlugInMgr = TyStPlugInMgr::GetInstance();
            const auto Menu = new QMenu;
            Menu->setFocus( Qt::ActiveWindowFocusReason );
            Menu->setFocusPolicy( Qt::StrongFocus );
            Menu->setFont( QFont( "Sarasa Mono K Light", 14 ) );
            Menu->deleteLater();

            StPlugInMgr->ConstructWLXMenu( Menu, filePath_ );

            bool IsLoadWLX = false;
            for( const auto& Ac : Menu->actions() )
            {
                TySpWLX Viewer = Ac->data().value< TySpWLX >();

                externalViewer_ = new CExternalViewer;
                externalViewer_->SetFilePath( filePath_ );
                externalViewer_->SetWLX( Viewer );

                if( externalViewer_->Initialize() == false )
                {
                    externalViewer_->deleteLater();
                    continue;
                }

                vbox_->addWidget( externalViewer_ );
                IsLoadWLX = true;
            }

            if( IsLoadWLX == false )
            {
                internalViewer_ = new CInternalViewer;
                internalViewer_->SetFilePath( filePath_ );
                vbox_->addWidget( internalViewer_ );
            }
            
            isInitOnce_ = true;
        }

    } while( false );

    CViewT::showEvent( event );
}

void CQuickView::SetContent(QtViewerHost& Host, std::shared_ptr<hc::storage::IContentSource> Source)
{
    if (!surface_)
    {
        surface_ = Host.CreateSurface(this); vbox_->addWidget(surface_); setFocusProxy(surface_);
        connect(surface_, &QtViewerSurface::CloseRequested, this, &CQuickView::CloseRequested);
        auto* Close = new QAction(tr("미리 보기 닫기"), this);
        Close->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Q)); Close->setShortcutContext(Qt::WidgetWithChildrenShortcut); addAction(Close);
        connect(Close, &QAction::triggered, this, &CQuickView::CloseRequested);
    }
    surface_->Open(std::move(Source), hc::viewer::ViewerPurpose::QuickPreview);
}

void CQuickView::CloseView() { if (surface_) surface_->Close(); }
void CQuickView::EnsureKeyboardFocusOnView() const { if (surface_) surface_->setFocus(); }

////int CmpPanel::InitializeGrid()
////{
////    Model->SetRoot( "C:" );
////    Model->SetCurrentPath( "\\" );
////    connect( Model, &FSModel::sigChangedDirectory, this, &CmpPanel::oo_ChangedDirectory );
////
////    Model->ChangeDirectory( QModelIndex() );
////    
////    return currentIndex;
////}
////


////void CmpPanel::ViewOnLister( const QModelIndex& SrcIndex )
////{
////    auto DlgViewer = new QDlgViewer;
////    DlgViewer->SetFileName( FileFullPath );
////    DlgViewer->SetInternalViewer( Viewer );
////    DlgViewer->show();
////}
////
