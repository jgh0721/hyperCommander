#include "stdafx.h"
#include "cmpView.hpp"
#include "UniqueLibs/columnAutoSelection.hpp"
#include "commonLibs/view/listIconSize.hpp"
#include <QtConcurrent/QtConcurrentRun>
#include <QFutureWatcher>
#include "UIs/dlgOpts.hpp"
#include "UniqueLibs/qtWorkspaceState.hpp"
#include "UniqueLibs/renameSelection.hpp"
#include "UniqueLibs/quickFindHighlight.hpp"
#include "UniqueLibs/commandUiConfig.hpp"

#include "builtInFsModel.hpp"
#include "cmnHCUtils.hpp"
#include "commandMgr.hpp"
#include "commandUiBridge.hpp"
#include "qtitanColumnAdapter.hpp"
#include "qtViewerHost.hpp"
#include "Modules/mdlFileModel.hpp"

#include <QtitanGrid.h>
#include <QtnGridViewGraphics.h>
#include <QtnGridEditors.h>
#include <QScopeGuard>
#include <limits>
#include <fmfilelist/ListPresentation.h>
#include <fmfilelist/ThumbnailView.h>
#include <fmfilelist/FileRoles.h>
#include "thumbnailModel.hpp"
#include <fmstyle/StylePaint.h>
#include <fmstyle/ThemeManager.h>

#include "QtitanMSVSDebug.h"

namespace {
using namespace fm::filelist;
using namespace fm::filelist::detail;
QIcon ProductIcon(const QVariant& Value)
{
    return Value.metaType() == QMetaType::fromType<QPixmap>() ? QIcon(Value.value<QPixmap>()) : Value.value<QIcon>();
}
// QIcon::paint never enlarges a smaller pixmap, so pixmaps are drawn to fill the icon slot.
void PaintListIcon(QPainter* Painter, const QVariant& Value, const QRect& Rect)
{
    if (Value.metaType() != QMetaType::fromType<QPixmap>()) { Value.value<QIcon>().paint(Painter, Rect); return; }
    Painter->save(); Painter->setRenderHint(QPainter::SmoothPixmapTransform);
    Painter->drawPixmap(Rect, Value.value<QPixmap>()); Painter->restore();
}
RecordState ProductRecordState(CGridView* Owner, int Row)
{
    RecordState State;
    State.active = Owner->IsPanelActive();
    State.cursor = Owner->BaseView()->focusedRowIndex() == Row;
    State.marked = Owner->BaseView()->modelController()->isRowSelected(Owner->BaseView()->getRow(Row));
    const auto Index = Owner->BaseView()->getRow(Row).modelIndex(0);
    const auto Opacity = Index.data(CFsModelT::USR_ROLE_HIDDENOPACITY);
    State.hidden = Opacity.isValid() && Opacity.toDouble() < 1.0;
    return State;
}
class ProductRecordPainter final : public Qtitan::GridRecordPainter {
public:
    explicit ProductRecordPainter(CGridView* Owner) : owner_(Owner) {}
    void paintBackground(QPainter& Painter, const Record& Value, QWidget*) override
    {
        const auto State = ProductRecordState(owner_, Value.rowIndex);
        const auto Group = Value.index.data(CFSModel::FileColorOverrideRole).toBool()
            ? Value.index.data(Qt::BackgroundRole).value<QColor>() : QColor{};
        paintRecordBackground(&Painter, Value.rect, owner_->RecordGeometry(), owner_->ListAppearance(),
            State, Value.rowIndex % 2 != 0, owner_->RecordColors(), Group);
    }
    void paintOverlay(QPainter& Painter, const Record& Value, QWidget*) override
    {
        const auto State = ProductRecordState(owner_, Value.rowIndex);
        if (owner_->IsTwoLine()) {
            // The icon spans both lines: centered on the record block and aligned with the cell padding.
            const auto Icon = Value.index.data(Qt::DecorationRole);
            const auto& Geometry = owner_->RecordGeometry();
            const int Size = owner_->ListIconSize();
            const auto Block = Geometry.block(Value.rect);
            Painter.save(); Painter.setOpacity(State.hidden ? 0.6 : 1.0);
            PaintListIcon(&Painter, Icon, QRect(Value.rect.left() + Geometry.sideMargin + 8, qRound(Block.center().y() - Size / 2.0), Size, Size));
            Painter.restore();
        }
        paintRecordCursor(&Painter, Value.rect, owner_->RecordGeometry(), owner_->ListAppearance(), State, owner_->RecordColors());
    }
private:
    CGridView* owner_;
};
class ProductCellDelegate final : public QStyledItemDelegate {
public:
    explicit ProductCellDelegate(CGridView* Owner, QObject* Parent) : QStyledItemDelegate(Parent), owner_(Owner) {}
    QWidget* createEditor(QWidget* Parent, const QStyleOptionViewItem& Option, const QModelIndex& Index) const override
    {
        auto* Editor = QStyledItemDelegate::createEditor(Parent, Option, Index);
        if (auto* Input = qobject_cast<QLineEdit*>(Editor)) {
            // Qtitan's adapter has a temporary model: update its editing value
            // on each user edit so drafts survive theme changes and cancel.
            connect(Input, &QLineEdit::textEdited, Input, [this, Input] {
                emit const_cast<ProductCellDelegate*>(this)->commitData(Input);
            });
        }
        return Editor;
    }
    void paint(QPainter* Painter, const QStyleOptionViewItem& Option, const QModelIndex& Index) const override
    {
        const auto Source = Index.data(Qtitan::QueryIndexRole).value<QModelIndex>();
        const auto Cell = Source.isValid() ? Source : Index;
        const auto Row = owner_->BaseView()->getRow(Cell);
        if (!Row.isValid()) return;
        const auto State = ProductRecordState(owner_, Row.rowIndex());
        const auto& Appearance = owner_->ListAppearance();
        const auto& Geometry = owner_->RecordGeometry();
        const auto& Colors = owner_->RecordColors();
        auto Text = textColors(Colors, Appearance, State, Geometry.twoLine, Geometry.separator);
        QFont Font = Cell.data(Qt::FontRole).value<QFont>();
        if (Font.family().isEmpty()) Font = Option.font;
        const bool Name = Cell.column() == 0;
        if (!Name) Font = fm::style::pixelFont(Font, std::max(8, Appearance.fontPx - 1), Font.weight());
        if (Text.bold && Name) Font.setBold(true);
        QColor Foreground = Name ? Text.name : Text.meta;
        if (Cell.siblingAtColumn(0).data(CFSModel::FileColorOverrideRole).toBool() && !State.marked)
            Foreground = Cell.data(Qt::ForegroundRole).value<QColor>();
        auto Rect = Option.rect.adjusted(8 + Geometry.sideMargin, 0, -8 - Geometry.sideMargin, 0);
        Painter->save(); Painter->setFont(Font); Painter->setPen(Foreground);
        const QFontMetrics Metrics(Font);
        if (Name) {
            const auto Icon = ProductIcon(Cell.data(Qt::DecorationRole));
            if (Geometry.twoLine) Rect.setLeft(Rect.left() + owner_->TwoLineTextIndent());
            else if (!Icon.isNull()) {
                const int Size = owner_->ListIconSize();
                const auto Block = Geometry.block(Option.rect);
                Painter->setOpacity(State.hidden ? 0.6 : 1.0);
                PaintListIcon(Painter, Cell.data(Qt::DecorationRole), QRect(Rect.left(), qRound(Block.center().y() - Size / 2.0), Size, Size));
                Painter->setOpacity(1.0); Rect.setLeft(Rect.left() + Size + hc::view::ListIconTextGap(false));
            }
            const bool Directory = (Cell.data(CFsModelT::USR_ROLE_ATTRIBUTE).toUInt() & FILE_ATTRIBUTE_DIRECTORY) != 0;
            const auto Shown = ElideFileName(Cell.data().toString(), Metrics, Rect.width(), Appearance.nameElide, Directory);
            const auto Query = owner_->property("hc.quickFind.text").toString();
            if (owner_->property("hc.quickFind.highlight").toBool() && !Query.isEmpty() &&
                hc::quickfind::Matches(Cell.data(Qt::EditRole).toString(), Query, owner_->property("hc.quickFind.match").toString()))
                hc::quickfind::PaintHighlightedText(Painter, Rect, Shown, Query);
            else Painter->drawText(Rect, Qt::AlignLeft | Qt::AlignVCenter, Shown);
        } else {
            const auto* Column = owner_->BaseView()->getColumnByModelColumn(Cell.column());
            const auto Alignment = static_cast<const Qtitan::GridTableColumn*>(Column)->textAlignment();
            if (Geometry.twoLine && Column->visualIndex() == 1) Rect.setLeft(Rect.left() + owner_->TwoLineTextIndent());
            Painter->drawText(Rect, Alignment | Qt::AlignVCenter, Metrics.elidedText(Cell.data().toString(), Qt::ElideRight, Rect.width()));
        }
        Painter->restore();
    }
private:
    CGridView* owner_;
};
}

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
    grid_->viewport()->installEventFilter(this);
    viewClickTimer_.setSingleShot(true);
    connect(&viewClickTimer_, &QTimer::timeout, this, [this] {
        const QModelIndex Index = mouseRenameIndex_; mouseRenameIndex_ = QPersistentModelIndex{};
        const auto Focus = QApplication::focusWidget();
        if (!panelActive_ || !Index.isValid() || IsEditing() || !Focus
            || (Focus != grid_ && !grid_->isAncestorOf(Focus)) || view_->focusedRow().modelIndex(0) != Index) return;
        if (auto* Bridge = TyStCommandMgr::GetInstance()->Bridge())
            Bridge->PromptFileOperation("hc.file.rename", "cell-click", Index);
    });
    renameTimer_.setInterval(30);
    connect(&renameTimer_, &QTimer::timeout, this, [this] {
        if (renameAwaitingRefresh_) continueRename();
        else if (renameEditor_) positionRenameEditor();
    });

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
    // Route header context events through the shared menu, including grouping.
    TableOpts.setColumnsQuickMenuVisible( false );
    
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
    connect( view_, &Qtitan::GridViewBase::editorStarting,      this, &CGridView::oo_grid_editorStarting );
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
    presentationStack_ = new QStackedWidget(this);
    thumbnails_ = new fm::filelist::ThumbnailView(this); thumbnails_->setObjectName("hc.thumbnails");
    thumbnails_->listView()->setObjectName("hc.thumbnailTiles"); thumbnails_->listView()->setModelColumn(0);
    thumbnailModel_ = new ThumbnailModel(this); thumbnails_->setModel(thumbnailModel_);
    thumbnails_->listView()->installEventFilter(this);
    presentationStack_->addWidget(grid_); presentationStack_->addWidget(thumbnails_);
    vbox_->addWidget(QueryWidget); vbox_->addWidget(presentationStack_, 1);
    connect(thumbnails_, &fm::filelist::ThumbnailView::paneActivated, this, &CViewT::sig_NotifyViewActivated);
    connect(thumbnails_, &fm::filelist::ThumbnailView::cursorRowChanged, this, [this](int Row) {
        if (syncingThumbnailCursor_ || Row < 0 || !IsThumbnailMode()) return;
        const auto NativeRow = view_->getRow(thumbnailModel_->SourceIndex(Row));
        if (NativeRow.isValid()) view_->setFocusedRowIndex(NativeRow.rowIndex());
    });
    connect(view_, &Qtitan::GridViewBase::focusRowChanged, this, [this] { SyncThumbnailCursor(); });
    connect(view_, &Qtitan::GridViewBase::selectionChanged, thumbnailModel_, &ThumbnailModel::Repaint);
    connect(view_, &Qtitan::GridViewBase::sortingChanged, thumbnailModel_, &ThumbnailModel::Schedule);
    connect(thumbnails_, &fm::filelist::ThumbnailView::activated, this, [this](const QModelIndex& Index) {
        QKeyEvent Event(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        TyStCommandMgr::GetInstance()->ProcessKeyPressEvent(&Event, thumbnailModel_->SourceIndex(Index.row()));
    });
    connect(thumbnails_, &fm::filelist::ThumbnailView::upRequested, this, [this] {
        QKeyEvent Event(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
        auto* Commands = TyStCommandMgr::GetInstance();
        if (!Commands->ProcessKeyPressEvent(&Event, retrieveCursorIndex()) && Commands->Bridge())
            Commands->Bridge()->ExecuteCommand("hc.navigate.parent", retrieveCursorIndex());
    });
    connect(thumbnails_, &fm::filelist::ThumbnailView::sizeChanged, this, [this](int Size) {
        thumbnailSizeOverride_ = Size; emit sigViewSettingsChanged(); requestPresentation();
    });
    connect(thumbnails_->listView()->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        if (!IsThumbnailMode()) return;
        // Reprioritize a new viewport: cached images survive; obsolete queued
        // extraction and running results are discarded before new tiles paint.
        thumbnailModel_->Provider()->CancelRequests();
        thumbnails_->listView()->viewport()->update();
    });
    thumbnails_->listView()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(thumbnails_->listView(), &QWidget::customContextMenuRequested, this, [this](const QPoint& Point) {
        const auto Tile = thumbnails_->listView()->indexAt(Point);
        if (Tile.isValid()) thumbnails_->setCursorRow(Tile.row());
        ShowShellMenu(Tile.isValid() ? thumbnailModel_->SourceIndex(Tile.row()) : QModelIndex{}, true);
    });
    connect(Stop, &QPushButton::clicked, this, [this] { if (auto* Model = qobject_cast<CFSModel*>(model_)) Model->CancelColumnQueries(); });
    connect(Resume, &QPushButton::clicked, this, [this] { if (auto* Model = qobject_cast<CFSModel*>(model_)) Model->ResumeColumnQueries(); });
    columnSortTimer_.setSingleShot(true);
    connect(view_, &Qtitan::GridViewBase::sortingChanged, this, [this] { columnSortTimer_.start(0); });
    connect(&columnSortTimer_, &QTimer::timeout, this, [this] {
        if (auto* Model = qobject_cast<CFSModel*>(model_)) {
            const auto Selected = CaptureSelection(); const auto Focus = CaptureFocus();
            const auto Scroll = CaptureScrollState();
            QScopedValueRollback GroupGuard(groupingProjection_, true);
            Model->SetColumnSorts(ColumnProfile().sorts_); applyGroupExpansion();
            RestoreSelection(Selected, Focus); RestoreScrollState(Scroll);
        }
    });
    setLayout( vbox_ );
    setSizePolicy( QSizePolicy( QSizePolicy::Policy::Expanding, QSizePolicy::Policy::Expanding ) );

    // Percentage/fixed widths are applied by QtitanColumnAdapter after header measurement.
    view_->bestFit( FitToHeader );
    columnAdapter_ = new QtitanColumnAdapter(this, grid_, view_);
    initializeGrouping();
    selectionKeyTimer_.setSingleShot(true); selectionKeyTimer_.setInterval(10);
    connect(&selectionKeyTimer_, &QTimer::timeout, this, &CGridView::drainSelectionKeys);
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState State) {
        if (State != Qt::ApplicationActive) finishSelectionInput();
    });
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* Focus) {
        if (Focus && Focus != grid_ && !grid_->isAncestorOf(Focus)) finishSelectionInput();
    });
    columnAdapter_->SetViewportHandlers([this] { return CaptureScrollState(); }, [this](const auto& State) { RestoreScrollState(State); });
    recordPainter_ = new ProductRecordPainter(this); view_->setRecordPainter(recordPainter_);
    selectionSummaryTimer_.setSingleShot(true);
    connect(&selectionSummaryTimer_, &QTimer::timeout, this, &CGridView::sigThumbnailSelectionChanged);
    presentationTimer_.setSingleShot(true); presentationTimer_.setInterval(80);
    connect(&presentationTimer_, &QTimer::timeout, this, &CGridView::applyPresentation);
    columnAdapter_->SetLayoutObserver([this](const QString& Mode) {
        if (!applyingPresentation_ && !restoringPresentation_) {
            listModeOverride_ = Mode == "details" ? "one-line" : "two-line";
            nameBelowOverride_ = Mode == "name-last";
            autoInitialized_ = false; requestPresentation();
        }
    });
}

CGridView::~CGridView()
{
    CancelQuickFind();
    pendingSelectionKeys_.clear(); selectionHeldKey_ = 0;
    if (auto* Native = qobject_cast<CFSModel*>(model_)) Native->SetSelectionInputActive(false);
    presentationTimer_.stop();
    automaticColumnsStop_.request_stop();
    thumbnailModel_->SetEnabled(false);
    view_->setRecordPainter(nullptr); delete recordPainter_;
}
void CGridView::CancelQuickFind()
{
    ++quickFindGeneration_;
    disconnect(quickFindPageConnection_); quickFindPageConnection_ = {};
}

void CGridView::MoveQuickFindCursor(const QString& Query, const QString& Match, int Direction)
{
    CancelQuickFind();
    pendingSelectionKeys_.clear(); selectionHeldKey_ = 0;
    if (auto* Native = qobject_cast<CFSModel*>(model_)) Native->SetSelectionInputActive(false);
    QPointer<CFSModel> Native = qobject_cast<CFSModel*>(model_);
    if (!Native || Query.isEmpty()) return;
    const auto Request = quickFindGeneration_;
    if (Native->IsLoading() || Native->IsPageLoading()) {
        quickFindPageConnection_ = connect(Native, &CFSModel::PageChanged, this, [this, Native, Request, Query, Match, Direction] {
            // Selection restoration follows the snapshot notification. Move
            // afterwards so it cannot overwrite the requested search cursor.
            QTimer::singleShot(0, this, [this, Native, Request, Query, Match, Direction] {
                if (Request == quickFindGeneration_ && Native && Native == model_ && !Native->IsLoading() && !Native->IsPageLoading())
                    MoveQuickFindCursor(Query, Match, Direction);
            });
        });
        return;
    }
    {
        const int Count = view_->getRowCount(); if (!Count) return;
        const int Current = view_->focusedRowIndex();
        const int Start = !Direction ? 0 : Current < 0 ? (Direction > 0 ? 0 : Count - 1) : (Current + (Direction > 0 ? 1 : Count - 1)) % Count;
        for (int Step = 0; Step < Count; ++Step) {
            const int Row = (Start + (Direction < 0 ? Count - Step : Step)) % Count;
            const auto ItemRow = view_->getRow(Row);
            if (ItemRow.type() != Qtitan::GridRow::DataRow) continue;
            const auto Visible = ItemRow.modelIndex(0);
            const auto Source = proxy_ ? proxy_->mapToSource(Visible) : Visible;
            const auto Item = Native->GetFileInfo(Source);
            if (!Item || !Item->storageEntry_ || !hc::quickfind::Matches(QString::fromStdString(Item->storageEntry_->name_), Query, Match)) continue;
            view_->setFocusedRowIndex(Row); view_->scrollToIndex(Visible, false); return;
        }
        return;
    }
}

bool CGridView::IsThumbnailMode() const { return presentationStack_ && presentationStack_->currentWidget() == thumbnails_; }
QPoint CGridView::ListContentTopLeft() const
{
    if (IsThumbnailMode()) return thumbnails_->listView()->viewport()->mapToGlobal(QPoint());
    // The rows panel starts immediately below the column headers, even when scrolled or empty.
    const auto* Rows = BaseView()->layout()->rowsPanel();
    return Rows->mapToGlobal(Rows->boundedRect().topLeft());
}
void CGridView::SyncThumbnailCursor()
{
    if (syncingThumbnailCursor_ || !IsThumbnailMode()) return;
    const auto Focus = view_->focusedRow().modelIndex(0);
    for (int Row = 0; Row < thumbnailModel_->rowCount(); ++Row)
        if (thumbnailModel_->SourceIndex(Row) == Focus) {
            if (thumbnails_->cursorRow() != Row) {
                QScopedValueRollback Guard(syncingThumbnailCursor_, true); thumbnails_->setCursorRow(Row);
            }
            break;
        }
}
void CGridView::ClearThumbnailCache() { thumbnailModel_->ClearCache(); }
void CGridView::hideEvent(QHideEvent* Event)
{
    CancelRename();
    thumbnailModel_->SetEnabled(false); CViewT::hideEvent(Event);
}
void CGridView::showEvent(QShowEvent* Event)
{
    CViewT::showEvent(Event); requestPresentation();
}
void CGridView::applyThumbnailPresentation()
{
    const auto Settings = hc::ui::ReadThumbnailSettings(hc::settings::AppSettings().Read(hc::settings::Section::General).toObject()["thumbnails"].toObject());
    thumbnailModel_->Apply(Settings);
    const bool Automatic = ListMode() == "auto" && Settings.autoSwitch_;
    if (ListMode() == "thumbnails" || Automatic) thumbnailModel_->Rebuild();
    const auto Percent = thumbnailModel_->TargetPercent();
    const bool Show = ListMode() == "thumbnails" || (Automatic && Percent >= Settings.autoPercent_);
    const bool Changed = Show != IsThumbnailMode();
    const bool HadFocus = grid_->hasFocus() || grid_->viewport()->hasFocus() || thumbnails_->listView()->hasFocus();
    auto Appearance = Settings.appearance_;
    if (thumbnailSizeOverride_) Appearance.size = *thumbnailSizeOverride_;
    if (Appearance.nameLines == 0) Appearance.nameLines = thumbnailModel_->FullNameLines(Appearance.tileWidth() - 20, fm::style::pixelFont(font(), 12.5));
    Appearance.invertCursor = listAppearance_.invertCursor; Appearance.invertSelection = listAppearance_.invertSelection;
    thumbnails_->setAppearance(Appearance); thumbnails_->setPaneActive(panelActive_);
    const auto Profile = ColumnProfile(); QStringList Sorts;
    for (const auto& Sort : Profile.sorts_) for (const auto& Column : Profile.columns_)
        if (Column.id_ == Sort.columnId_) Sorts.append(Column.title_ + (Sort.order_ == Qt::AscendingOrder ? " ↑" : " ↓"));
    thumbnails_->setSortText(Sorts.isEmpty() ? tr("모델 순서") : Sorts.join(" · "));
    presentationStack_->setCurrentWidget(Show ? static_cast<QWidget*>(thumbnails_) : grid_);
    if (Changed) groupingTimer_.start(0);
    thumbnailModel_->SetEnabled(Show && isVisible());
    setProperty("hc.thumbnailTargetPercent", Percent); SyncThumbnailCursor();
    if (Changed) {
        if (HadFocus) EnsureKeyboardFocusOnView();
        emit sigListPresentationChanged();
    }
}
bool CGridView::IsTwoLine() const { return columnAdapter_ && columnAdapter_->LayoutMode() != "details"; }
int CGridView::ListIconSize() const
{
    return hc::view::ListIconSize(recordGeometry_.pitch - recordGeometry_.gap, recordGeometry_.twoLine);
}
int CGridView::TwoLineTextIndent() const { return ListIconSize() + hc::view::ListIconTextGap(true); }
QString CGridView::ListMode() const
{
    return listModeOverride_.isEmpty() ? hc::ui::ReadFilePanelSettings(
        hc::settings::AppSettings().Read(hc::settings::Section::General).toObject().value("filePanel").toObject()).mode_ : listModeOverride_;
}
bool CGridView::SetListMode(const QString& Mode)
{
    if (!hc::ui::ListModes.contains(Mode) || !columnAdapter_->Writable() || !unsupportedPresentation_.isEmpty()) return false;
    listModeOverride_ = Mode == "inherit" ? QString{} : Mode;
    nameBelowOverride_.reset(); autoInitialized_ = false;
    applyPresentation(); emit sigListPresentationChanged(); emit sigViewSettingsChanged();
    return true;
}
QJsonObject CGridView::capturePresentation() const
{
    if (!unsupportedPresentation_.isEmpty()) return unsupportedPresentation_;
    QJsonObject Result{{"version", 1}, {"mode", ListModeOverride()}};
    if (nameBelowOverride_) Result["nameBelow"] = *nameBelowOverride_;
    if (thumbnailSizeOverride_) Result["thumbnailSize"] = *thumbnailSizeOverride_;
    return Result;
}
void CGridView::restorePresentation(const QJsonObject& View)
{
    unsupportedPresentation_ = {}; thumbnailSizeOverride_.reset(); listModeOverride_.clear(); nameBelowOverride_.reset(); autoInitialized_ = false;
    const auto Value = View.value("listPresentation").toObject();
    if (!Value.isEmpty()) {
        if (Value.value("version") != QJsonValue(1) || !hc::ui::ListModes.contains(Value.value("mode").toString()) ||
            (Value.contains("nameBelow") && !Value.value("nameBelow").isBool())) unsupportedPresentation_ = Value;
        else {
            listModeOverride_ = Value.value("mode").toString() == "inherit" ? QString{} : Value.value("mode").toString();
            if (Value.contains("nameBelow")) nameBelowOverride_ = Value.value("nameBelow").toBool();
            const int Size = Value.value("thumbnailSize").toInt();
            if (Size == 64 || Size == 96 || Size == 160 || Size == 256) thumbnailSizeOverride_ = Size;
        }
    } else if (View.contains(hc::columns::WorkspaceKey)) {
        // Older workspaces persisted a resolved layout. Keep that manual choice.
        const auto Mode = columnAdapter_->LayoutMode();
        listModeOverride_ = Mode == "details" ? "one-line" : "two-line";
        nameBelowOverride_ = Mode == "name-last";
    }
    requestPresentation();
}
void CGridView::requestPresentation()
{
    if (!presentationTimer_.isActive()) presentationTimer_.start();
}
void CGridView::applyPresentation()
{
    requestAutomaticColumns();
    if (!columnAdapter_ || !model_ || !isVisible()) return;
    if (IsEditing()) { presentationTimer_.start(150); return; }
    applyThumbnailPresentation();
    const auto Settings = hc::ui::ReadFilePanelSettings(
        hc::settings::AppSettings().Read(hc::settings::Section::General).toObject().value("filePanel").toObject());
    auto Appearance = Settings.appearance_;
    Appearance.invertCursor = listAppearance_.invertCursor; Appearance.invertSelection = listAppearance_.invertSelection;
    if (nameBelowOverride_) Appearance.nameBelow = *nameBelowOverride_;
    Appearance.fontPx = QFontMetrics(grid_->font()).height() > 0
        ? (grid_->font().pixelSize() > 0 ? grid_->font().pixelSize() : qRound(grid_->font().pointSizeF() * logicalDpiY() / 72.0)) : 13;
    const auto Location = model_->property("hc.snapshotLocation").toString();
    if (autoLocation_ != Location) { autoLocation_ = Location; autoInitialized_ = false; }
    bool Two = ListMode() == "two-line";
    const int Available = columnAdapter_->AvailableWidth();
    if (!Available) return;
    if (ListMode() == "auto") {
        const auto* Source = proxy_ ? static_cast<QAbstractItemModel*>(proxy_) : static_cast<QAbstractItemModel*>(model_);
        const QFontMetrics Metrics(grid_->font());
        const int Count = std::min(Source->rowCount(), 2048);
        int Clipped = 0, Measured = 0;
        const int NameWidth = std::max(0, columnAdapter_->OneLineNameWidth(Available) - 38);
        for (int Sample = 0; Sample < Count; ++Sample) {
            const int Row = int(qint64(Sample) * Source->rowCount() / std::max(1, Count));
            const auto Text = Source->index(Row, 0).data().toString();
            if (Text == "..") continue;
            ++Measured; if (Metrics.horizontalAdvance(Text) > NameWidth) ++Clipped;
        }
        const double Percent = Measured ? 100.0 * Clipped / Measured : 0;
        Two = fm::filelist::ResolveAutoTwoLine(Available, Percent, Appearance.autoWidth, Appearance.autoPercent, autoTwoLine_, autoInitialized_);
        autoInitialized_ = true; autoTwoLine_ = Two;
        setProperty("hc.autoClippedPercent", Percent);
    }
    if (!unsupportedPresentation_.isEmpty() || !columnAdapter_->Writable()) {
        Two = IsTwoLine(); Appearance.nameBelow = columnAdapter_->LayoutMode() == "name-last";
    }
    const auto NewMode = Two ? (Appearance.nameBelow ? "name-last" : "name-first") : "details";
    const auto NewGeometry = fm::filelist::detail::RecordGeometry::make(recordColors_.isWatercolor(), Two, Appearance);
    const bool Changed = columnAdapter_->LayoutMode() != NewMode || recordGeometry_.pitch != NewGeometry.pitch ||
        recordGeometry_.nameBelow != NewGeometry.nameBelow;
    const auto Anchor = Changed ? columnAdapter_->CaptureViewport() : QtitanColumnAdapter::ViewportAnchor{};
    QScopedValueRollback Guard(applyingPresentation_, true);
    if (unsupportedPresentation_.isEmpty() && columnAdapter_->Writable() && columnAdapter_->LayoutMode() != NewMode)
        columnAdapter_->SetLayoutMode(NewMode);
    if (auto* Native = qobject_cast<CFSModel*>(model_))
        Native->SetColumnLayoutMode(columnAdapter_->Profile().mode_);
    listAppearance_ = Appearance; recordGeometry_ = NewGeometry;
    // Load icons at the drawn size so 48–128 px icons are not upscaled from 16 px.
    if (auto* Native = qobject_cast<CFSModel*>(model_))
        Native->SetIconPixels(qCeil(ListIconSize() * grid_->devicePixelRatioF()));
    view_->beginUpdate();
    view_->options().setRowAutoHeight(false);
    view_->options().setCellHeight(recordGeometry_.cellHeight());
    view_->options().setGroupRowHeight(QFontMetrics(grid_->font()).height() + 12);
    auto GroupPalette = grid_->palette();
    GroupPalette.setColor(QPalette::All, QPalette::Base, recordColors_[fm::style::Token::Head]);
    GroupPalette.setColor(QPalette::All, QPalette::Window, recordColors_[fm::style::Token::Head]);
    GroupPalette.setColor(QPalette::All, QPalette::BrightText, recordColors_[fm::style::Token::Fg]);
    GroupPalette.setColor(QPalette::All, QPalette::Text, recordColors_[fm::style::Token::Fg]);
    GroupPalette.setColor(QPalette::All, QPalette::Dark, recordColors_[fm::style::Token::Head]);
    GroupPalette.setColor(QPalette::All, QPalette::Highlight, recordColors_[fm::style::Token::InvCur]);
    GroupPalette.setColor(QPalette::All, QPalette::HighlightedText, recordColors_[fm::style::Token::OnInvCur]);
    view_->options().setGroupPalette(0, GroupPalette);
    view_->options().setColumnHeight(std::max(QFontMetrics(grid_->font()).height() + 4,
        recordGeometry_.twoLine ? recordGeometry_.headerHeight / 2 : recordGeometry_.headerHeight));
    for (int Index = 0; Index < model_->columnCount(); ++Index)
        if (auto* Column = view_->getColumnByModelColumn(Index))
            static_cast<Qtitan::GridTableColumn*>(Column)->setCaptionIndent(Two && (Index == 0 || Column->visualIndex() == 1) ? TwoLineTextIndent() : 0);
    view_->endUpdate();
    columnAdapter_->Apply();
    if (Changed) columnAdapter_->RestoreViewport(Anchor);
    grid_->viewport()->update();
    if (Changed) emit sigListPresentationChanged();
}

void CGridView::SetModel( CFsModelT* Source, QAbstractProxyModel* Proxy, ColumnView* Columns )
{
    pendingSelectionKeys_.clear(); selectionHeldKey_ = 0;
    if (auto* Native = qobject_cast<CFSModel*>(model_)) Native->SetSelectionInputActive(false);
    CancelRename();
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

    appliedDisplaySettings_ = {};
    model_ = Source;
    proxy_ = Proxy;
    thumbnailModel_->Bind(Proxy ? static_cast<QAbstractItemModel*>(Proxy) : Source);
    // HC-QTN-005: 8.2.0 can retain selection ranges across a model reset.
    // Clear them before rows disappear: selectedRowIndexes otherwise dereferences
    // an invalid GridRow, and old row positions must not target different files.
    disconnect(selectionReset_);
    selectionReset_ = connect(Source, &QAbstractItemModel::modelAboutToBeReset,
        this, [this] {
            resetRenameClicks(); groupingReset_ = true;
            resetSelection_ = CaptureSelection(); resetFocus_ = CaptureFocus();
            resetPath_ = model_->property("hc.snapshotLocation").toString();
            model_->setProperty("hc.selectAllEntries", false);
            QScopedValueRollback GroupGuard(groupingProjection_, true); view_->deselectAll();
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
        connect(Native, &CFSModel::ColumnValuesChanging, this, [this] {
            if (groupingUpdateDepth_++ == 0) {
                querySelection_ = CaptureSelection(); queryFocus_ = CaptureFocus(); queryScroll_ = CaptureScrollState(); view_->beginUpdate();
            }
        });
        connect(Native, &CFSModel::ColumnValuesChanged, this, [this] {
            if (groupingUpdateDepth_ > 0 && --groupingUpdateDepth_ == 0) {
                const bool Restore = !groupingProjection_;
                QScopedValueRollback GroupGuard(groupingProjection_, true); view_->endUpdate();
                if (Restore) { applyGroupExpansion(); RestoreSelection(querySelection_, queryFocus_); RestoreScrollState(queryScroll_); }
            }
        });
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
    if (!Source->property("hc.presentationSignals").toBool()) {
        Source->setProperty("hc.presentationSignals", true);
        const auto Schedule = [this] { requestPresentation(); };
        connect(Source, &QAbstractItemModel::modelReset, this, Schedule);
        connect(Source, &QAbstractItemModel::modelReset, this, [this] { groupingTimer_.start(0); });
        connect(Source, &QAbstractItemModel::rowsInserted, this, Schedule);
        connect(Source, &QAbstractItemModel::dataChanged, this, Schedule);
        if (Proxy) {
            connect(Proxy, &QAbstractItemModel::modelReset, this, Schedule);
            connect(Proxy, &QAbstractItemModel::layoutChanged, this, Schedule);
            connect(Proxy, &QAbstractItemModel::rowsInserted, this, Schedule);
        }
    }

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
            GridColumn->setEditorType(Qtitan::GridEditor::DelegateAdapter);
            static_cast<Qtitan::GridDelegateAdapterEditorRepository*>(GridColumn->editorRepository())
                ->setDelegate(new ProductCellDelegate(this, GridColumn->editorRepository()));
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
    groupingTimer_.start(0);
    if (auto* Native = qobject_cast<CFSModel*>(model_)) Native->SetColumnSorts(Columns->profile_.sorts_);
    ApplyDisplaySettings();
    requestPresentation();
}

void CGridView::ApplyDisplaySettings()
{
    if (!model_ || !view_) return;
    const auto General = hc::settings::AppSettings().Read(hc::settings::Section::General).toObject();
    // A theme-only notification must not refresh/sort the entire controller:
    // doing so can close an editor and move the viewport.
    requestPresentation();
    auto Display = General; Display.remove("filePanel");
    if (Display == appliedDisplaySettings_) return;
    appliedDisplaySettings_ = Display;
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

    // Qtitan reloads the active editor even for appearance-only dataChanged.
    // Only the file-name column is editable here; retain its draft and caret.
    QPointer<Qtitan::GridEditor> Editor = view_->activeEditor();
    QPointer<QLineEdit> Input = Editor ? qobject_cast<QLineEdit*>(Editor->getCellWidget()) : nullptr;
    const auto Draft = Input ? Input->text() : QString{};
    const int Cursor = Input ? Input->cursorPosition() : 0;
    const int SelectionStart = Input ? Input->selectionStart() : -1;
    const int SelectionLength = Input ? Input->selectedText().size() : 0;
    const bool Modified = Editor && Editor->isContextModified();
    const auto RestoreEditor = qScopeGuard([&] {
        if (!Input || !Editor || view_->activeEditor() != Editor) return;
        Input->setText(Draft);
        if (Modified) emit Input->textEdited(Draft);
        Input->setCursorPosition(Cursor);
        if (SelectionStart >= 0) {
            const bool Backward = Cursor == SelectionStart;
            Input->setSelection(Backward ? SelectionStart + SelectionLength : SelectionStart,
                Backward ? -SelectionLength : SelectionLength);
        }
    });

    if( model_ != nullptr )
        model_->SetColorScheme( ColorScheme );

    ApplyDisplaySettings();
    // RowFocus 색상 등 적용
    view_->beginUpdate();
    auto& BaseOpts = view_->options();
    grid_->setFont(ColorScheme.FileList_Font);
    thumbnails_->setFont(ColorScheme.FileList_Font);
    thumbnails_->listView()->setPalette(QApplication::palette());
    recordColors_ = fm::style::themeColorsFor(this);
    recordColors_.setColor(fm::style::Token::Surface, ColorScheme.FileList_BGColor);
    recordColors_.setColor(fm::style::Token::Fg, ColorScheme.FileList_FGColor);
    // Legacy defaults used a neutral mark color. The record painter uses
    // white-on-accent selection; only explicit user colors override its tokens.
    if (ColorScheme.explicitColors_.contains("listSelection")) {
        recordColors_.setColor(fm::style::Token::Sel, ColorScheme.FileList_Selected);
        recordColors_.setColor(fm::style::Token::InvSel, ColorScheme.FileList_Selected);
    }
    if (ColorScheme.explicitColors_.contains("listCursor")) recordColors_.setColor(fm::style::Token::InvCur, ColorScheme.FileList_Cursor);
    listAppearance_.invertCursor = ColorScheme.IsInverseCursor;
    listAppearance_.invertSelection = ColorScheme.IsInverseSelect;
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

    syncViewportBackground();

    if( ColorScheme.FileList_Selected.isValid() == true )
        BaseOpts.setSelectedBackgroundColor( ColorScheme.FileList_Selected );

    if( ColorScheme.FileList_Cursor.isValid() == true )
        BaseOpts.setRowFocusColor( ColorScheme.FileList_Cursor );

    view_->endUpdate();
    requestPresentation();
}

void CGridView::SetPanelActive(bool Active)
{
    if (!Active) { resetRenameClicks(); finishSelectionInput(); }
    panelActive_ = Active;
    thumbnails_->setPaneActive(Active);
    grid_->viewport()->update();
    // Keep the logical cursor and marks intact; only suppress cursor painting.
    if (view_ && view_->options().showFocusDecoration() != Active)
        view_->options().setShowFocusDecoration(Active);
}

bool CGridView::IsEditing() const
{
    if (renameEditor_ || renameAwaitingRefresh_) return true;
    if( view_ == nullptr )
        return false;

    return view_->activeEditor() != nullptr;
}

bool CGridView::BeginRename(const QModelIndex& Index, bool SelectExtension, RenameHandler Handler)
{
    resetRenameClicks();
    if (renameEditor_) {
        if (!renameSubmitting_ && !renameAwaitingRefresh_)
            selectRenameText(renameEditor_->selectedText() != renameEditor_->text());
        return true;
    }
    auto* Native = qobject_cast<CFSModel*>(model_);
    if (!Native || Native->IsLoading() || !Native->ProviderEnabled() || IsThumbnailMode()) return false;
    auto Source = Index.isValid() ? Index : retrieveCursorIndex();
    while (const auto* Proxy = qobject_cast<const QAbstractProxyModel*>(Source.model())) Source = Proxy->mapToSource(Source);
    if (!Source.isValid() || Source.model() != model_) return false;
    const auto Item = model_->GetFileInfo(Source);
    if (!Item || Item->Name_ == "..") return false;
    if (view_->activeEditor()) view_->hideEditor();
    renameIndex_ = Source.siblingAtColumn(0);
    renameOriginal_ = Item->Name_; renameLocation_ = model_->GetCurrentPathWithRoot();
    renameHandler_ = std::move(Handler); renameSelectExtension_ = SelectExtension;
    renameSubmitting_ = renameAwaitingRefresh_ = renameRetry_ = false; renameDirection_ = 0;
    ++renameGeneration_;
    const auto Visible = proxy_ ? proxy_->mapFromSource(renameIndex_) : QModelIndex(renameIndex_);
    const auto Row = view_->getRow(Visible);
    if (!Row.isValid()) { CancelRename(); return false; }
    view_->setFocusedRowIndex(Row.rowIndex());
    view_->scrollToIndex(Visible, false);
    renameEditor_ = new QLineEdit(renameOriginal_, grid_->viewport());
    renameEditor_->setObjectName("hc.file.renameEditor");
    renameEditor_->setAccessibleName(tr("파일 이름 변경"));
    renameEditor_->setToolTip(tr("이름 변경 단축키: 선택 전환 · 위/아래: 완료 후 이전/다음 항목 · Esc: 취소"));
    renameEditor_->setFont(grid_->font());
    renameEditor_->installEventFilter(this);
    positionRenameEditor();
    if (!renameEditor_) return false;
    renameEditor_->show(); renameEditor_->raise(); renameEditor_->setFocus(Qt::ShortcutFocusReason);
    selectRenameText(SelectExtension); renameTimer_.start();
    return true;
}

void CGridView::CancelRename()
{
    resetRenameClicks();
    ++renameGeneration_;
    renameTimer_.stop();
    if (renameEditor_) {
        renameEditor_->removeEventFilter(this); renameEditor_->hide(); renameEditor_->deleteLater(); renameEditor_ = nullptr;
    }
    renameIndex_ = QPersistentModelIndex{}; renameHandler_ = {};
    renameSubmitting_ = renameAwaitingRefresh_ = renameRetry_ = false;
}

void CGridView::selectRenameText(bool IncludeExtension)
{
    if (renameEditor_) hc::ui::SelectRenameText(renameEditor_, IncludeExtension);
}

void CGridView::positionRenameEditor()
{
    if (!renameEditor_ || renameSubmitting_ || renameAwaitingRefresh_) return;
    if (!renameIndex_.isValid() || model_->GetCurrentPathWithRoot() != renameLocation_) { CancelRename(); return; }
    const auto Visible = proxy_ ? proxy_->mapFromSource(renameIndex_) : QModelIndex(renameIndex_);
    const auto Row = view_->getRow(Visible);
    for (const auto& Hit : grid_->hitInfoAll()) {
        if (Hit.info() != Qtitan::GridHitInfo::Cell || Hit.row().rowIndex() != Row.rowIndex() || Hit.modelIndex().column() != 0) continue;
        auto Rect = Hit.rect().adjusted(2, 1, -2, -1);
        if (IsTwoLine()) Rect.setHeight(qMin(Rect.height(), renameEditor_->sizeHint().height()));
        Rect.setWidth(qMin(qMax(Rect.width(), 120), grid_->viewport()->width() - Rect.left()));
        renameEditor_->setGeometry(Rect); return;
    }
    // A scrolled-away editor must not remain over a different file.
    CancelRename();
}

void CGridView::commitRename(int Direction)
{
    if (!renameEditor_ || renameSubmitting_ || renameAwaitingRefresh_ || !renameIndex_.isValid()) return;
    const auto Name = renameEditor_->text();
    renameDirection_ = Direction; renameNext_ = Name;
    if (Direction) {
        renameNext_.clear();
        const auto Visible = proxy_ ? proxy_->mapFromSource(renameIndex_) : QModelIndex(renameIndex_);
        const auto Current = view_->getRow(Visible);
        for (int Row = Current.rowIndex() + Direction; Row >= 0 && Row < view_->getRowCount(); Row += Direction) {
            const auto ItemRow = view_->getRow(Row);
            if (ItemRow.type() != Qtitan::GridRow::DataRow) continue;
            auto At = ItemRow.modelIndex(0);
            if (proxy_) At = proxy_->mapToSource(At);
            const auto Entry = model_->GetFileInfo(At);
            if (Entry && Entry->Name_ != "..") { renameNext_ = Entry->Name_; break; }
        }
    }
    renameSubmitting_ = true; renameEditor_->setReadOnly(true);
    const auto Generation = renameGeneration_;
    if (Name == renameOriginal_) { finishRename(true, Generation); return; }
    // Freeze the source index before dispatch; never use whichever row gains focus later.
    const auto Handler = renameHandler_;
    Handler(renameIndex_, Name, [Guard = QPointer<CGridView>(this), Generation](bool Success) {
        if (Guard) Guard->finishRename(Success, Generation);
    });
}

void CGridView::finishRename(bool Success, quint64 Generation)
{
    if (Generation != renameGeneration_ || !renameEditor_) return;
    renameSubmitting_ = false; renameAwaitingRefresh_ = true; renameRetry_ = !Success;
    if (!Success) renameNext_ = renameOriginal_;
    renameEditor_->hide();
    renameTimer_.start();
}

void CGridView::continueRename()
{
    auto* Native = qobject_cast<CFSModel*>(model_);
    if (!Native || model_->GetCurrentPathWithRoot() != renameLocation_ || !isVisible()) { CancelRename(); return; }
    if (Native->IsLoading()) return;
    QModelIndex Target;
    for (int Row = 0; Row < model_->rowCount(); ++Row)
        if (model_->GetName(model_->index(Row, 0)) == renameNext_) { Target = model_->index(Row, 0); break; }
    const auto Handler = renameHandler_;
    const auto SelectExtension = renameSelectExtension_;
    const bool Continue = renameDirection_ != 0 && !renameRetry_;
    const bool Retry = renameRetry_;
    const auto Draft = renameEditor_ ? renameEditor_->text() : QString{};
    CancelRename();
    if (Target.isValid()) {
        const auto Visible = proxy_ ? proxy_->mapFromSource(Target) : Target;
        const auto Row = view_->getRow(Visible);
        if (Row.isValid()) { view_->setFocusedRowIndex(Row.rowIndex()); view_->scrollToIndex(Visible, false); }
        if ((Continue || Retry) && BeginRename(Target, SelectExtension, Handler) && Retry) {
            renameEditor_->setText(Draft); selectRenameText(SelectExtension);
        }
    }
    if (!renameEditor_) EnsureKeyboardFocusOnView();
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
    if (!restoringPresentation_ && !applyingAutoColumns_) {
        manualColumns_ = true; automaticColumnsBase_.reset(); automaticColumnsStop_.request_stop(); ++automaticColumnsGeneration_;
    }
    if (!restoringPresentation_) {
        listModeOverride_ = Profile.mode_ == hc::columns::LayoutMode::Details ? "one-line" : "two-line";
        nameBelowOverride_ = Profile.mode_ == hc::columns::LayoutMode::NameLast;
    }
    view_->beginUpdate();
    QScopedValueRollback GroupGuard(groupingProjection_, true);
    view_->deselectAll(); view_->setModel(nullptr); view_->removeColumns();
    auto Binding = BindColumnProfile(Profile);
    SetModel(model_, proxy_, &Binding);
    columnAdapter_->UseProfileWidths();
    columnAdapter_->Apply();
    view_->endUpdate();
    groupingTimer_.start(0); RestoreSelection(Selected, Focus);
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
    Layout["manualSelection"] = manualColumns_;
    if (automaticColumnsBase_) Layout["automaticBase"] = hc::columns::EncodeProfile(*automaticColumnsBase_);
    if (columnAdapter_->Writable()) Layout["definition"] = hc::columns::EncodeProfile(ColumnProfile());
    return {{"groupingV1", unsupportedGrouping_.isEmpty() ? hc::columns::EncodeGrouping(grouping_) : unsupportedGrouping_}, {"listPresentation", capturePresentation()}, {"mode", "details"}, {hc::columns::WorkspaceKey, Layout}, {"columnOrderV2", Columns},
        {"hiddenSystem", Proxy && Proxy->GetHiddenSystem()}, {"search", Proxy ? Proxy->Search() : QString{}}, {"searchMatch", Proxy ? Proxy->SearchMatch() : QString{}}};
}

void CGridView::RestoreWorkspaceView(const QJsonObject& View)
{
    QScopedValueRollback RestoreGuard(restoringPresentation_, true);
    const auto SavedGrouping = View.value("groupingV1").toObject();
    const auto DecodedGrouping = hc::columns::DecodeGrouping(SavedGrouping);
    if (DecodedGrouping) { grouping_ = *DecodedGrouping; unsupportedGrouping_ = {}; }
    else { grouping_ = {}; unsupportedGrouping_ = SavedGrouping; }
    const auto Layout = View.value(hc::columns::WorkspaceKey).toObject();
    manualColumns_ = Layout["manualSelection"].toBool(Layout.contains("definition"));
    automaticColumnsBase_ = hc::columns::DecodeProfile(Layout["automaticBase"].toObject());
    automaticColumnsSignature_.clear(); automaticColumnsStop_.request_stop(); ++automaticColumnsGeneration_;
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
    restorePresentation(View); groupingTimer_.start(0);
    if (auto* Proxy = qobject_cast<CFSProxyModel*>(proxy_))
    { Proxy->SetHiddenSystem(View.value("hiddenSystem").toBool()); Proxy->SetSearch(View.value("search").toString(), View.value("searchMatch").toString()); }
}

QStringList CGridView::CaptureSelection() const
{
    if (GroupingActive()) return groupedSelection_.values();
    QStringList Result;
    for (const auto& Index : SelectedSourceIndexes()) {
        const auto Key = sourceSelectionKey(Index); if (!Key.isEmpty()) Result.append(Key);
    }
    return Result;
}

QString CGridView::CaptureFocus() const
{
    if (!model_ || !view_) return {};
    const auto Row = view_->focusedRow(); if (!Row.isValid()) return {};
    if (Row.type() == Qtitan::GridRow::GroupRow) return "@group:" + Row.groupValue().toString();
    return sourceSelectionKey(Row.modelIndex(0));
}

QJsonObject CGridView::CaptureScrollState() const
{
    if (!view_ || !model_) return {};
    auto* Layout = static_cast<Qtitan::GridViewBase*>(view_)->layout(); Layout->getModelHeight();
    const int Scroll = view_->verticalScrollBar()->value();
    auto State = QJsonObject{{"version", 1}, {"y", Scroll}, {"x", view_->horizontalScrollBar()->value()}};
    if (!view_->getRowCount()) return State;
    int Low = 0, High = view_->getRowCount();
    while (Low + 1 < High) {
        const int Middle = (Low + High) / 2;
        if (Layout->getRowScrollPos(Middle) <= Scroll) Low = Middle; else High = Middle;
    }
    const auto Row = view_->getRow(Low);
    const bool Group = Row.type() == Qtitan::GridRow::GroupRow;
    State["kind"] = Group ? "group" : "entry";
    State["anchor"] = Group ? Row.groupValue().toString() : sourceSelectionKey(Row.modelIndex(0));
    State["offset"] = Scroll - Layout->getRowScrollPos(Low);
    return State;
}

void CGridView::RestoreScrollState(const QJsonObject& State)
{
    if (State.isEmpty() || !view_ || !model_) return;
    auto* Layout = static_cast<Qtitan::GridViewBase*>(view_)->layout(); Layout->updateLayout(); Layout->getModelHeight();
    int Position = -1;
    const auto Anchor = State["anchor"].toString();
    if (State["kind"] == QJsonValue("group")) {
        for (const auto& Group : view_->modelController()->groupRows()) if (Group._groupValue.toString() == Anchor) { Position = Group.rowIndex; break; }
    } else if (!Anchor.isEmpty()) for (const auto& Index : SelectionCandidates()) {
        if (sourceSelectionKey(Index) != Anchor) continue;
        const auto Visible = proxy_ ? proxy_->mapFromSource(Index) : Index;
        const auto Row = view_->getRow(Visible); if (Row.isValid()) Position = Row.rowIndex();
        break;
    }
    int Scroll = State["y"].toInt();
    if (Position >= 0 && Position < view_->getRowCount()) {
        const auto Row = view_->getRow(Position);
        const int Offset = std::clamp(State["offset"].toInt(), 0, (std::max)(0, Layout->calculateGridRowLength(Row, false) - 1));
        Scroll = Layout->getRowScrollPos(Position) + Offset;
    }
    view_->verticalScrollBar()->setValue(Scroll); view_->horizontalScrollBar()->setValue(State["x"].toInt());
}

void CGridView::RestoreSelection(const QStringList& Selection, const QString& Focus)
{
    if (!model_ || !view_) return;
    const QSet<QString> Keys(Selection.begin(), Selection.end());
    const auto Previous = groupedSelection_;
    QScopedValueRollback Guard(groupingProjection_, true);
    view_->beginUpdate(); view_->deselectAll();
    if (GroupingActive()) groupedSelection_.clear();
    int FocusRow = -1;
    for (const auto& Source : SelectionCandidates()) {
        const auto Key = sourceSelectionKey(Source);
        const bool Selected = !Key.isEmpty() && Keys.contains(Key);
        if (!Selected && Key != Focus) continue;
        const auto Visible = proxy_ ? proxy_->mapFromSource(Source) : Source;
        if (Selected && GroupingActive()) groupedSelection_.insert(Key);
        const auto Row = view_->getRow(Visible); if (!Row.isValid()) continue;
        if (Selected && Row.type() == Qtitan::GridRow::DataRow) view_->selectRow(Row.rowIndex(), Qtitan::Select);
        if (!Key.isEmpty() && Key == Focus) FocusRow = Row.rowIndex();
    }
    if (Focus.startsWith("@group:")) for (const auto& Group : view_->modelController()->groupRows())
        if (Group._groupValue.toString() == Focus.mid(7)) { FocusRow = Group.rowIndex; break; }
    if (FocusRow >= 0) view_->setFocusedRowIndex(FocusRow);
    view_->endUpdate();
    if (GroupingActive() && Previous != groupedSelection_) emit sigSelectionIdentityChanged();
    selectionSummaryTimer_.start(0);
}

void CGridView::EnsureKeyboardFocusOnView() const
{
    if( view_ == nullptr )
        return;

    if (IsThumbnailMode()) { thumbnails_->listView()->setFocus(Qt::MouseFocusReason); return; }
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

    if( view_ == nullptr )
        return {};
    if (IsGroupFocused()) {
        if (IsMoveDown) view_->navigateDown(Qt::ControlModifier);
        return {};
    }
    if (!SrcIndex.isValid()) return {};
    const auto Row = view_->getRow( SrcIndex );
    if (!Row.isValid() || Row.type() != Qtitan::GridRow::DataRow) return {};
    const auto ModelIndex = proxy_ != nullptr ? proxy_->mapToSource( SrcIndex ) : SrcIndex;
    nsHC::TySpFileSource Info = model_->GetFileInfo( ModelIndex );

    if (!Info || Info->Name_ == "..") {
        if (IsMoveDown) view_->navigateDown(Qt::ControlModifier);
        return {};
    }
    if (GroupingActive()) {
        const auto Key = sourceSelectionKey(ModelIndex); if (Key.isEmpty()) return {};
        IsSelected = !groupedSelection_.contains(Key);
        if (IsSelected) groupedSelection_.insert(Key); else groupedSelection_.remove(Key);
        {
            QScopedValueRollback Guard(groupingProjection_, true);
            view_->selectRow(Row.rowIndex(), IsSelected ? Qtitan::Select : Qtitan::Unselect);
        }
        emit sigSelectionIdentityChanged(); selectionSummaryTimer_.start(0);
    } else {
        view_->selectRow(Row.rowIndex(), Qtitan::Invert);
        IsSelected = view_->modelController()->isRowSelected(Row);
    }
    if (IsSelected && Info && Info->Name_ != ".." && (Info->Attributes_ & FILE_ATTRIBUTE_DIRECTORY)
        && hc::settings::AppSettings().CommittedObject()["options"].toObject()["folderSizeOnSpace"].toBool(true))
        if (auto* Native = qobject_cast<CFSModel*>(model_)) Native->RequestDirectorySizes({ModelIndex});
    
    if( IsMoveDown == true )
        view_->navigateDown( Qt::ControlModifier );

    return Info;
}

void CGridView::queueSelectionKey(QKeyCombination Key)
{
    if (!model_) return;
    if (pendingSelectionKeys_.isEmpty() && !processingSelectionKey_) {
        selectionKeyModel_ = model_;
        selectionKeyLocation_ = model_->property("hc.snapshotLocation").toString();
    }
    pendingSelectionKeys_.append(Key); syncSelectionInputActivity();
    drainSelectionKeys();
}

void CGridView::drainSelectionKeys()
{
    // COM and Qtitan can pump native input during command dispatch. A nested
    // repeat must resolve its file index after the preceding cursor move.
    if (processingSelectionKey_) return;
    QPointer<CGridView> Alive(this);
    processingSelectionKey_ = true;
    const auto Finish = qScopeGuard([Alive] {
        if (Alive) { Alive->processingSelectionKey_ = false; Alive->syncSelectionInputActivity(); }
    });
    while (!pendingSelectionKeys_.isEmpty()) {
        if (!panelActive_ || IsEditing() || !model_ || model_ != selectionKeyModel_
            || model_->property("hc.snapshotLocation").toString() != selectionKeyLocation_) {
            pendingSelectionKeys_.clear(); selectionKeyTimer_.stop(); return;
        }
        const auto* Native = qobject_cast<CFSModel*>(model_);
        if (groupingProjection_ || groupingReset_ || groupingUpdateDepth_ || groupInteractionPending_
            || (Native && Native->IsLoading())) {
            selectionKeyTimer_.start(); return;
        }
        const auto Key = pendingSelectionKeys_.takeFirst();
        QKeyEvent Event(QEvent::KeyPress, Key.key(), Key.keyboardModifiers());
        TyStCommandMgr::GetInstance()->ProcessKeyPressEvent(&Event, retrieveCursorIndex());
        if (!Alive) return;
    }
    selectionKeyTimer_.stop();
}

void CGridView::finishSelectionInput()
{
    selectionHeldKey_ = 0;
    syncSelectionInputActivity();
}

void CGridView::syncSelectionInputActivity()
{
    if (auto* Native = qobject_cast<CFSModel*>(model_))
        Native->SetSelectionInputActive(selectionHeldKey_ != 0 || processingSelectionKey_ || !pendingSelectionKeys_.isEmpty());
}

void CGridView::ContextMenuOnCurrentTab( const QModelIndex& SrcIndex )
{
    ShowShellMenu(SrcIndex);
}

QStringList CGridView::ShellMenuPaths(const QModelIndex& Index, bool UseSelection) const
{
    const auto* Native = qobject_cast<CFSModel*>(model_);
    if (!Native || Native->IsLoading() || !view_) return {};
    const auto Kind = Native->WorkspaceLocation().kind_;
    if (!Native->IsSearchResults() && Kind != hc::storage::LocatorKind::Local && Kind != hc::storage::LocatorKind::Smb) return {};
    const auto SourceIndex = [](QModelIndex At) {
        while (const auto* Proxy = qobject_cast<const QAbstractProxyModel*>(At.model())) At = Proxy->mapToSource(At);
        return At;
    };
    QStringList Paths;
    const auto Append = [&](const QModelIndex& At) {
        const auto Source = SourceIndex(At);
        if (!Source.isValid() || Source.model() != model_) return;
        const auto Item = model_->GetFileInfo(Source);
        if (Item && Item->Name_ != "..") Paths.append(Native->GetFileFullPath(Source));
    };
    if (UseSelection)
        for (const auto& Source : SelectedSourceIndexes()) Append(Source);
    if (Paths.isEmpty()) Append(Index.isValid() ? Index : retrieveCursorIndex());
    if (Paths.isEmpty()) Paths.append(model_->GetCurrentPathWithRoot());
    for (auto& Path : Paths) Path = QDir::fromNativeSeparators(Path);
    Paths.removeDuplicates();
    return Paths;
}

bool CGridView::ShowShellMenu(const QModelIndex& Index, bool AtCursor)
{
    if (IsEditing()) return false;
    if (IsGroupFocused() && !Index.isValid() && CaptureSelection().isEmpty()) return false;
    auto Source = Index.isValid() ? Index : retrieveCursorIndex();
    while (const auto* Proxy = qobject_cast<const QAbstractProxyModel*>(Source.model())) Source = Proxy->mapToSource(Source);
    if (Source.isValid() && Source.model() != model_) return false;
    const auto Visible = proxy_ ? proxy_->mapFromSource(Source) : Source;
    const auto Row = view_->getRow(Visible);
    const bool UseSelection = !AtCursor || (Index.isValid() && Row.isValid() && view_->modelController()->isRowSelected(Row));
    auto Paths = ShellMenuPaths(Source, UseSelection);
    if (Paths.isEmpty()) return false;
    if (AtCursor && !Index.isValid()) Paths = {model_->GetCurrentPathWithRoot()};
    POINT Position{};
    if (AtCursor) {
        if (!GetCursorPos(&Position)) return false;
    } else {
        auto* Viewport = grid_->viewport();
        QPoint Anchor(8, 8);
        if (IsThumbnailMode()) {
            Viewport = thumbnails_->listView()->viewport();
            const auto Rect = thumbnails_->listView()->visualRect(thumbnails_->listView()->currentIndex());
            if (Rect.isValid()) Anchor = Rect.bottomLeft();
        } else if (Row.isValid()) {
            view_->scrollToIndex(Visible, false);
            for (const auto& Hit : grid_->hitInfoAll())
                if (Hit.info() == GridHitInfo::Cell && Hit.row().rowIndex() == Row.rowIndex() && Hit.modelIndex().column() == 0) {
                    Anchor = Hit.rect().bottomLeft(); break;
                }
        }
        Anchor.setX(qBound(0, Anchor.x(), qMax(0, Viewport->width() - 1)));
        Anchor.setY(qBound(0, Anchor.y(), qMax(0, Viewport->height() - 1)));
        const auto Scale = Viewport->devicePixelRatioF();
        Position = {qRound(Anchor.x() * Scale), qRound(Anchor.y() * Scale)};
        if (!ClientToScreen(reinterpret_cast<HWND>(Viewport->winId()), &Position)) return false;
    }
    viewClickTimer_.stop();
    return OpenShellContextMenuForObjects(Paths, Position.x, Position.y, reinterpret_cast<void*>(window()->winId()));
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
    if (Type == GridHitInfo::Column || Type == GridHitInfo::GroupRow || Type == GridHitInfo::GroupBranchButton) { showGroupingMenu(Args); return; }
    if (Type != GridHitInfo::Cell && Type != GridHitInfo::Rows) return;
    ShowShellMenu(Type == GridHitInfo::Cell ? Info.modelIndex() : QModelIndex{}, true);
    Args->setHandled( true );
}

void CGridView::resetRenameClicks()
{
    viewClickTimer_.stop();
    mouseRenameIndex_ = QPersistentModelIndex{};
    lastNameClickIndex_ = QPersistentModelIndex{};
    lastNameClickTime_.invalidate();
}

void CGridView::oo_grid_cellClicked( Qtitan::CellClickEventArgs* Args )
{
    const auto& Cell = Args->cell();
    if (!panelActive_ || !plainLeftClick_ || Cell.modelIndex().column() != 0 || IsEditing()
        || !hc::settings::AppSettings().Read(hc::settings::Section::Options).toObject()["renameOnSlowClick"].toBool(true)) {
        lastNameClickIndex_ = QPersistentModelIndex{}; lastNameClickTime_.invalidate(); return;
    }
    const auto Index = Cell.modelIndex();
    const int Interval = QApplication::doubleClickInterval();
    const auto Elapsed = lastNameClickTime_.isValid() ? lastNameClickTime_.elapsed() : -1;
    // Selection alone never requests editing. Require two fresh name clicks,
    // slower than a double-click but within three double-click intervals.
    if (lastNameClickIndex_ == Index && Elapsed >= Interval && Elapsed <= 3 * Interval) {
        mouseRenameIndex_ = Index;
        viewClickTimer_.start(Interval);
    }
    lastNameClickIndex_ = Index; lastNameClickTime_.start();
}

void CGridView::oo_grid_rowDblClicked( Qtitan::RowClickEventArgs* Args )
{
    auto Row = Args->row();
    if (Row.isValid() && Row.type() == Qtitan::GridRow::GroupRow) { Row.setGroupExpanded(!Row.isGroupExpanded()); Args->setHandled(true); return; }
    const auto Index = Row.modelIndex( 0 );
    const auto StCommandMgr = TyStCommandMgr::GetInstance();

    if( view_->activeEditor() != nullptr )
        view_->closeEditor();

    resetRenameClicks();

    QKeyEvent Event( QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier );

    Args->setHandled( StCommandMgr->ProcessKeyPressEvent( &Event, Index ) );
}

void CGridView::oo_grid_rowFocusChanged( int OldRowIndex, int NewRowIndex )
{
    if (OldRowIndex != NewRowIndex) resetRenameClicks();
    if (renameEditor_ && !renameSubmitting_ && !renameAwaitingRefresh_ && OldRowIndex != NewRowIndex)
        CancelRename();
    UNREFERENCED_PARAMETER( OldRowIndex );
    UNREFERENCED_PARAMETER( NewRowIndex );

    ////qDebug() << qMakePair( OldRowIndex, NewRowIndex );

    viewClickTimer_.stop();

    //for( const auto& State : vecTabStates )
    //    State->LastFocusedRowIndex = -1;
    //
    //retrieveFocusState()->LastFocusedRowIndex = OldRowIndex;
}

void CGridView::oo_grid_editorStarting( Qtitan::EditorEventArgs* Args )
{
    const auto Policy = Args->activationPolicy();
    const auto MousePolicies = GridEditor::ActivateByMousePress | GridEditor::ActivateByClick | GridEditor::ActivateByDblClick;
    if (!(Policy & MousePolicies)) return;
    viewClickTimer_.stop(); mouseRenameIndex_ = QPersistentModelIndex{};
    if (Args->dataBinding()->column() != 0) return;
    // Mouse edits must use the product editor and its rename policies.
    Args->setHandled(true);
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
    if (GroupingActive() && !groupingProjection_ && !groupInteractionPending_ && !groupingReset_ && !groupingUpdateDepth_) {
        const auto Before = groupedSelection_;
        QSet<QString> Old, Next;
        for (const auto& Index : OldSelection->selectedRowIndexes()) { const auto Key = sourceSelectionKey(Index); if (!Key.isEmpty()) Old.insert(Key); }
        for (const auto& Index : NewSelection->selectedRowIndexes()) { const auto Key = sourceSelectionKey(Index); if (!Key.isEmpty()) Next.insert(Key); }
        for (const auto& Key : Old) if (!Next.contains(Key)) groupedSelection_.remove(Key);
        groupedSelection_.unite(Next);
        if (Before != groupedSelection_) emit sigSelectionIdentityChanged();
    }
    if (model_) {
        model_->setProperty("hc.selectAllEntries", false);
        // Publish after legacy panel toggle bookkeeping and reset restoration.
        // One queued summary also covers select-all and command-driven marks.
        selectionSummaryTimer_.start(0);
    }
    UNREFERENCED_PARAMETER( NewSelection );
    UNREFERENCED_PARAMETER( OldSelection );

    viewClickTimer_.stop();

    //for( const auto& State : vecTabStates )
    //    State->LastFocusedRowIndex = -1;
}

void CGridView::syncViewportBackground()
{
    auto Palette = grid_->palette();
    Palette.setColor(QPalette::All, grid_->backgroundRole(), recordColors_[fm::style::Token::Surface]);
    grid_->setPalette(Palette);
    grid_->viewport()->setPalette(Palette);
    grid_->viewport()->setAutoFillBackground(true);
    grid_->viewport()->update();
}

bool CGridView::eventFilter( QObject* Watched, QEvent* Event )
{
    if (Event->type() == QEvent::KeyRelease && (Watched == grid_ || Watched == grid_->viewport())) {
        const auto* Key = static_cast<QKeyEvent*>(Event);
        if (!Key->isAutoRepeat() && Key->key() == selectionHeldKey_) finishSelectionInput();
    }
    if (Event->type() == QEvent::KeyPress && (Watched == grid_ || Watched == grid_->viewport()) && !IsEditing()) {
        const auto* Key = static_cast<QKeyEvent*>(Event);
        if (hc::ui::CommandId(TyStCommandMgr::GetInstance()->Retrieve().value(Key->keyCombination())) == "hc.selection.invert") {
            Event->accept(); resetRenameClicks();
            if (!Key->isAutoRepeat()) selectionHeldKey_ = Key->key();
            queueSelectionKey(Key->keyCombination()); return true;
        }
    }
    if (Watched == grid_->viewport()
        && (Event->type() == QEvent::MouseButtonPress || Event->type() == QEvent::MouseButtonDblClick)) {
        const auto* Mouse = static_cast<QMouseEvent*>(Event);
        viewClickTimer_.stop(); mouseRenameIndex_ = QPersistentModelIndex{};
        renamePressPosition_ = Mouse->position().toPoint();
        plainLeftClick_ = Event->type() == QEvent::MouseButtonPress
            && Mouse->button() == Qt::LeftButton && Mouse->modifiers() == Qt::NoModifier;
        const auto Hit = grid_->hitInfo(renamePressPosition_);
        if (!plainLeftClick_ || Hit.info() != Qtitan::GridHitInfo::Cell
            || Hit.modelIndex() != lastNameClickIndex_) resetRenameClicks();
    }
    if (Watched == grid_->viewport() && Event->type() == QEvent::MouseMove && plainLeftClick_) {
        const auto* Mouse = static_cast<QMouseEvent*>(Event);
        if (Mouse->buttons().testFlag(Qt::LeftButton)
            && (Mouse->position().toPoint() - renamePressPosition_).manhattanLength() >= QApplication::startDragDistance()) {
            plainLeftClick_ = false; resetRenameClicks();
        }
    }
    if (renameEditor_ && Watched == renameEditor_) {
        if (Event->type() == QEvent::KeyPress || Event->type() == QEvent::ShortcutOverride) {
            auto* Key = static_cast<QKeyEvent*>(Event);
            const bool RenameKey = hc::ui::CommandId(TyStCommandMgr::GetInstance()->Retrieve().value(Key->keyCombination())) == "hc.file.rename";
            const bool MoveKey = !RenameKey && (Key->key() == Qt::Key_Up || Key->key() == Qt::Key_Down);
            const bool Special = RenameKey || Key->key() == Qt::Key_Escape || MoveKey
                || Key->key() == Qt::Key_Return || Key->key() == Qt::Key_Enter;
            if (Special && Event->type() == QEvent::ShortcutOverride) { Event->accept(); return true; }
            if (Special) {
                // Each new editor must accept the held arrow key's repeat events.
                // Keep selection toggles and completion/cancellation single-shot.
                if (!Key->isAutoRepeat() || MoveKey) {
                    if (Key->key() == Qt::Key_Escape) { CancelRename(); EnsureKeyboardFocusOnView(); }
                    else if (RenameKey) { if (!renameSubmitting_ && !renameAwaitingRefresh_) selectRenameText(renameEditor_->selectedText() != renameEditor_->text()); }
                    else commitRename(Key->key() == Qt::Key_Up ? -1 : Key->key() == Qt::Key_Down ? 1 : 0);
                }
                return true;
            }
        }
    }
    if (Event->type() == QEvent::KeyPress && (Watched == grid_ || Watched == grid_->viewport()) && IsGroupFocused()) {
        const auto* Key = static_cast<QKeyEvent*>(Event);
        if ((Key->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier
            && (Key->key() == Qt::Key_Return || Key->key() == Qt::Key_Enter || Key->key() == Qt::Key_Left || Key->key() == Qt::Key_Right)) {
            auto Row = view_->focusedRow();
            Row.setGroupExpanded(Key->key() == Qt::Key_Right ? true : Key->key() == Qt::Key_Left ? false : !Row.isGroupExpanded());
            return true;
        }
    }
    if ((Event->type() == QEvent::DragMove || Event->type() == QEvent::Drop) && (Watched == grid_ || Watched == grid_->viewport())) {
        const auto* Drop = static_cast<QDropEvent*>(Event);
        const auto Info = grid_->hitInfo(grid_->viewport()->mapFrom(static_cast<QWidget*>(Watched), Drop->position().toPoint())).info();
        if (Info == Qtitan::GridHitInfo::GroupRow || Info == Qtitan::GridHitInfo::GroupBranchButton) { Event->ignore(); return true; }
    }
    // Changing the main window frame stylesheet on maximize repolishes children,
    // which resets viewport auto-fill. Restore it after stylesheet processing.
    if (Watched == grid_ && Event->type() == QEvent::StyleChange)
        QTimer::singleShot(0, this, &CGridView::syncViewportBackground);
    // A DPI change reloads icons at the new physical size.
    if (Event->type() == QEvent::FontChange || Event->type() == QEvent::Show || Event->type() == QEvent::DevicePixelRatioChange) requestPresentation();
    if (Watched == grid_->viewport() && Event->type() == QEvent::KeyPress) resetRenameClicks();

    if( Event->type() == QEvent::KeyPress && Watched != grid_->viewport() )
    {
        resetRenameClicks();
        if( IsEditing() == true )
            return false;

        const auto KeyEvent = dynamic_cast< QKeyEvent* >( Event );
        const auto StCommandMgr = TyStCommandMgr::GetInstance();

        qDebug() << __FUNCTION__ << " : " << KeyEvent << KeyEvent->text();

        if( StCommandMgr->ProcessKeyPressEvent( KeyEvent, retrieveCursorIndex() ) == true )
            return true;
        // Configured commands take priority over the legacy Ctrl+A fallback.
        if (KeyEvent->matches(QKeySequence::SelectAll) && qobject_cast<CFSModel*>(model_)) {
            if (GroupingActive()) {
                QStringList Keys; for (const auto& Index : SelectionCandidates()) { const auto Key = sourceSelectionKey(Index); if (!Key.isEmpty()) Keys.append(Key); }
                RestoreSelection(Keys, CaptureFocus());
            } else view_->selectAll();
            model_->setProperty("hc.selectAllEntries", true);
            if (IsThumbnailMode()) emit sigThumbnailSelectionChanged();
            return true;
        }
    }

    if( Event->type() == QEvent::FocusOut )
    {
        if( Watched == grid_ || Watched == grid_->viewport() )
        {
            resetRenameClicks();
        }
    }

    return CViewT::eventFilter( Watched, Event );
}

void CGridView::resizeEvent( QResizeEvent* event )
{
    if (columnAdapter_) columnAdapter_->Request();
    requestPresentation();

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
    if (!Row.isValid() || Row.type() != Qtitan::GridRow::DataRow) return {};

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

void CGridView::SetAutomaticColumns(bool Automatic)
{
    manualColumns_ = !Automatic; automaticColumnsSignature_.clear();
    automaticColumnsStop_.request_stop(); ++automaticColumnsGeneration_;
    requestPresentation(); emit sigViewSettingsChanged();
}
void CGridView::requestAutomaticColumns()
{
    auto* Model = qobject_cast<CFSModel*>(model_);
    if (!Model || Model->IsLoading() || Model->HasPlaceholder() || !columnAdapter_ || !columnAdapter_->Writable()
        || manualColumns_ || applyingAutoColumns_ || IsEditing()) return;
    const auto Committed = hc::settings::AppSettings().CommittedObject();
    const auto Columns = Committed["columns"].toObject();
    const auto Policy = Columns[hc::columns::AutoSelectionKey].toObject();
    const auto Library = Columns["ColumnsV2/Profiles"].toObject();
    const auto Colors = Committed["options"].toObject()["fileColorRules"].toObject();
    const auto Path = QString::fromStdString(Model->WorkspaceLocation().root_);
    const auto Signature = QByteArray::number(Model->DirectoryGeneration()) + Path.toUtf8() + QJsonDocument(Policy).toJson(QJsonDocument::Compact) + QJsonDocument(Library).toJson(QJsonDocument::Compact) + QJsonDocument(Colors).toJson(QJsonDocument::Compact);
    if (Signature == automaticColumnsSignature_) return;
    automaticColumnsSignature_ = Signature; automaticColumnsStop_.request_stop(); automaticColumnsStop_ = std::stop_source{};
    const auto Generation = ++automaticColumnsGeneration_;
    using AutoResult = hc::storage::Result<std::optional<hc::columns::ColumnProfile>>;
    auto* Watcher = new QFutureWatcher<AutoResult>(this);
    const auto DirectoryGeneration = Model->DirectoryGeneration();
    connect(Watcher, &QFutureWatcher<AutoResult>::finished, this, [this, Watcher, Generation, DirectoryGeneration] {
        const auto Result = Watcher->result(); Watcher->deleteLater();
        auto* Current = qobject_cast<CFSModel*>(model_);
        if (Generation != automaticColumnsGeneration_ || manualColumns_ || !Current || Current->IsLoading() || Current->DirectoryGeneration() != DirectoryGeneration) return;
        if (IsEditing()) { automaticColumnsSignature_.clear(); requestPresentation(); return; }
        if (!Result.HasValue()) { setProperty("hc.columns.autoError", QString::fromStdString(Result.Failure().code_)); return; }
        setProperty("hc.columns.autoError", QVariant{});
        auto Next = Result.Value();
        if (Next && !automaticColumnsBase_) automaticColumnsBase_ = ColumnProfile();
        if (!Next && automaticColumnsBase_) Next = automaticColumnsBase_;
        if (Next && hc::columns::EncodeProfile(*Next) != hc::columns::EncodeProfile(ColumnProfile())) {
            const QScopedValueRollback Guard(applyingAutoColumns_, true);
            ApplyColumnProfile(*Next); emit sigViewSettingsChanged();
        }
        if (!Result.Value()) automaticColumnsBase_.reset();
    });
    const auto Index = Model->DirectoryEntries();
    hc::storage::OperationContext Context; Context.stopToken_ = automaticColumnsStop_.get_token();
    Watcher->setFuture(QtConcurrent::run([Policy, Library, Colors, Path, Index, Context] {
        return hc::columns::SelectAutomaticColumns(Policy, Library, Colors, Path, Index, Context);
    }));
}
