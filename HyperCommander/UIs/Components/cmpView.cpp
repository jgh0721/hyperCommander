#include "stdafx.h"
#include "cmpView.hpp"
#include "UniqueLibs/productDefaults.hpp"
#include "startupTrace.hpp"
#include "UniqueLibs/columnAutoSelection.hpp"
#include "UniqueLibs/inputGate.hpp"
#include "commonLibs/view/listIconSize.hpp"
#include <QtConcurrent/QtConcurrentRun>
#include <QFutureWatcher>
#include "UIs/dlgOpts.hpp"
#include "UniqueLibs/qtWorkspaceState.hpp"
#include "UniqueLibs/renameSelection.hpp"
#include "UniqueLibs/quickFindHighlight.hpp"
#include "UniqueLibs/commandUiConfig.hpp"
#include "UniqueLibs/errorPresentation.hpp"
#include "UniqueLibs/fileRulePaint.hpp"

#include "builtInFsModel.hpp"
#include "cmnHCUtils.hpp"
#include "commandMgr.hpp"
#include "commandUiBridge.hpp"
#include "audio/audioSession.hpp"
#include "qtitanColumnAdapter.hpp"
#include "qtViewerHost.hpp"
#include "UniqueLibs/qtPluginHostSettings.hpp"
#include "UniqueLibs/document/documentOpenController.hpp"
#include "commonLibs/document/documentFormatSupport.hpp"
#include "Modules/mdlFileModel.hpp"
#include "UIs/cmpPanel.hpp"

#include <QtitanGrid.h>
#include <QtnGridViewGraphics.h>
#include <QtnGridEditors.h>
#include <QScopeGuard>
#include <QDrag>
#include <QDragEnterEvent>
#include <QContextMenuEvent>
#include <QWidgetAction>
#include <fmstyle/Glyphs.h>
#include <QMimeData>
#include "UniqueLibs/fileDragMime.hpp"
#include <QUrl>
#include <QLabel>
#include <limits>
#include <fmfilelist/ListPresentation.h>
#include <fmfilelist/ThumbnailView.h>
#include <fmfilelist/FileRoles.h>
#include "thumbnailModel.hpp"
#include "qtitanGrouping.hpp"
#include <fmstyle/StylePaint.h>
#include <fmstyle/ThemeManager.h>

#include "QtitanMSVSDebug.h"

namespace {
using namespace fm::filelist;
using namespace fm::filelist::detail;
QString FilePanelDragToken(QWidget* View)
{
    QObject* Owner = View;
    for (auto* Parent = View; Parent; Parent = Parent->parentWidget())
        if (auto* Panel = qobject_cast<CmpPanel*>(Parent)) { Owner = Panel; break; }
    auto Token = Owner->property("hc.filePanelDragToken").toString();
    if (Token.isEmpty()) { Token = QUuid::createUuid().toString(QUuid::WithoutBraces); Owner->setProperty("hc.filePanelDragToken", Token); }
    return Token;
}
bool ProductHasIcon(const QVariant& Value)
{
    return Value.metaType() == QMetaType::fromType<QPixmap>() ? !Value.value<QPixmap>().isNull() : !Value.value<QIcon>().isNull();
}
// Use the painted filename bounds for overlays as well as normal cell text.
QRect ProductNameTextRect(const CGridView* Owner, const QRect& Cell, bool HasIcon)
{
    const auto& Geometry = Owner->RecordGeometry();
    auto Rect = Cell.adjusted(8 + Geometry.sideMargin, 0, -8 - Geometry.sideMargin, 0);
    if (Geometry.twoLine) Rect.setLeft(Rect.left() + Owner->TwoLineTextIndent());
    else if (HasIcon) Rect.setLeft(Rect.left() + Owner->ListIconSize() + hc::view::ListIconTextGap(false));
    return Rect;
}
// QIcon::paint never enlarges a smaller pixmap, so pixmaps are drawn to fill the icon slot.
void PaintListIcon(QPainter* Painter, const QVariant& Value, const QRect& Rect)
{
    if (Value.metaType() != QMetaType::fromType<QPixmap>()) { Value.value<QIcon>().paint(Painter, Rect); return; }
    Painter->save(); Painter->setRenderHint(QPainter::SmoothPixmapTransform);
    Painter->drawPixmap(Rect, Value.value<QPixmap>()); Painter->restore();
}
// 제목표시줄에서 재생 중인 파일: 아이콘 자리에 막대 셋(재생) 또는 둘(일시정지)을 그린다.
// Animate면 막대가 0.9초 주기로 오르내린다(시안 B안). 뷰의 타이머가 그 칸만 다시 그린다.
void PaintNowPlaying(QPainter* Painter, const QRect& Rect, bool Playing, const QColor& Color, bool Animate)
{
    Painter->save(); Painter->setRenderHint(QPainter::Antialiasing); Painter->setPen(Qt::NoPen); Painter->setBrush(Color);
    const qreal Unit = Rect.width() / 14.0, Bottom = Rect.top() + 12 * Unit;
    if (Playing) {
        qreal Heights[] = {6, 9, 4.5};
        if (Animate) {
            static QElapsedTimer Clock; if (!Clock.isValid()) Clock.start();
            const auto Cycle = (Clock.elapsed() % 900) / 900.0;
            for (int Bar = 0; Bar < 3; ++Bar)
                Heights[Bar] = 9 * (0.35 + 0.65 * (0.5 - 0.5 * std::cos(qDegreesToRadians(360.0 * (Cycle + Bar / 3.0)))));
        }
        for (int Bar = 0; Bar < 3; ++Bar)
            Painter->drawRoundedRect(QRectF(Rect.left() + (2 + Bar * 3.8) * Unit, Bottom - Heights[Bar] * Unit, 2.4 * Unit, Heights[Bar] * Unit), 0.6 * Unit, 0.6 * Unit);
    } else {
        for (const qreal X : {3.6, 8.0}) Painter->drawRoundedRect(QRectF(Rect.left() + X * Unit, Rect.top() + 3 * Unit, 2.4 * Unit, 8 * Unit), 0.6 * Unit, 0.6 * Unit);
    }
    Painter->restore();
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
        owner_->RecordStartupPaint(Value.index,1);
    }
    void paintOverlay(QPainter& Painter, const Record& Value, QWidget*) override
    {
        const auto State = ProductRecordState(owner_, Value.rowIndex);
        const auto Effects = Value.index.data(CFSModel::FileEffectsRole).toJsonObject();
        if (owner_->IsTwoLine()) {
            // The icon spans both lines: centered on the record block and aligned with the cell padding.
            const auto Icon = Value.index.data(Qt::DecorationRole);
            const auto& Geometry = owner_->RecordGeometry();
            const int Size = owner_->ListIconSize();
            const auto Block = Geometry.block(Value.rect);
            Painter.save(); Painter.setOpacity(std::min({Value.index.data(CFSModel::ClipboardCutRole).toBool() ? 0.5 : 1.0,
                State.hidden ? 0.6 : 1.0, Effects["dim"].toBool() ? 0.55 : 1.0}));
            PaintListIcon(&Painter, Icon, QRect(Value.rect.left() + Geometry.sideMargin + 8, qRound(Block.center().y() - Size / 2.0), Size, Size));
            Painter.restore();
        }
        paintRecordCursor(&Painter, Value.rect, owner_->RecordGeometry(), owner_->ListAppearance(), State, owner_->RecordColors());
        Painter.save(); if (Effects["dim"].toBool()) Painter.setOpacity(Painter.opacity() * 0.55);
        hc::ui::PaintFileRuleBorder(&Painter, Value.rect, Value.index.data(Qt::ForegroundRole).value<QColor>(), Effects);
        hc::ui::PaintFileRulePending(&Painter, Value.rect, Effects);
        owner_->RecordStartupPaint(Value.index,4);
        Painter.restore();
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
        const int NowPlaying = Name ? Cell.data(CFSModel::NowPlayingRole).toInt() : 0;
        if (NowPlaying) Font.setBold(true);
        QColor Foreground = Name ? Text.name : Text.meta;
        if (Cell.siblingAtColumn(0).data(CFSModel::FileColorOverrideRole).toBool() && !State.marked)
            Foreground = Cell.data(Qt::ForegroundRole).value<QColor>();
        auto Rect = Option.rect.adjusted(8 + Geometry.sideMargin, 0, -8 - Geometry.sideMargin, 0);
        Painter->save(); Painter->setFont(Font); Painter->setPen(Foreground);
        const auto Effects = Cell.data(CFSModel::FileEffectsRole).toJsonObject();
        const QFontMetrics Metrics(Font);
        if (Name) {
            const auto Icon = Cell.data(Qt::DecorationRole);
            const bool HasIcon = ProductHasIcon(Icon);
            if (!Geometry.twoLine && HasIcon) {
                const int Size = owner_->ListIconSize();
                const auto Block = Geometry.block(Option.rect);
                const QRect IconRect(Rect.left(), qRound(Block.center().y() - Size / 2.0), Size, Size);
                if (NowPlaying) PaintNowPlaying(Painter, IconRect, NowPlaying == 1, Foreground, owner_->NowPlayingAnimating());
                else {
                    Painter->setOpacity(std::min({Cell.data(CFSModel::ClipboardCutRole).toBool() ? 0.5 : 1.0,
                        State.hidden ? 0.6 : 1.0, Effects["dim"].toBool() ? 0.55 : 1.0}));
                    PaintListIcon(Painter, Icon, IconRect);
                    Painter->setOpacity(1.0);
                }
            }
            Rect = ProductNameTextRect(owner_, Option.rect, HasIcon);
            const bool Directory = (Cell.data(CFsModelT::USR_ROLE_ATTRIBUTE).toUInt() & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (owner_->EverythingPresentation()) {
                const auto Prefix = QStringLiteral("→ ");
                Painter->drawText(Rect, Qt::AlignLeft | Qt::AlignVCenter, Prefix);
                Rect.setLeft(Rect.left() + Metrics.horizontalAdvance(Prefix));
            }
            const auto Shown = ElideFileName(Cell.data().toString(), Metrics, Rect.width(), Appearance.nameElide, Directory);
            if (Effects["dim"].toBool()) Painter->setOpacity(0.55);
            const auto Query = owner_->property("hc.quickFind.text").toString();
            if (owner_->property("hc.quickFind.highlight").toBool() && !Query.isEmpty() &&
                hc::quickfind::Matches(Cell.data(Qt::EditRole).toString(), Query, owner_->property("hc.quickFind.match").toString()))
                hc::quickfind::PaintHighlightedText(Painter, Rect, Shown, Query, Qt::AlignLeft, owner_->property("hc.quickFind.match").toString());
            else Painter->drawText(Rect, Qt::AlignLeft | Qt::AlignVCenter, Shown);
        } else {
            if (Effects["dim"].toBool()) Painter->setOpacity(0.55);
            const auto* Column = owner_->BaseView()->getColumnByModelColumn(Cell.column());
            const auto Alignment = static_cast<const Qtitan::GridTableColumn*>(Column)->textAlignment();
            if (Geometry.twoLine && Column->visualIndex() == 1) Rect.setLeft(Rect.left() + owner_->TwoLineTextIndent());
            const bool Path = owner_->EverythingPresentation() && Column->property("hc.everything.path").toBool();
            Painter->drawText(Rect, Alignment | Qt::AlignVCenter, Metrics.elidedText(Cell.data().toString(), Path ? Qt::ElideMiddle : Qt::ElideRight, Rect.width()));
        }
        Painter->restore();
        if(Name)owner_->RecordStartupPaint(Cell,2);
    }
private:
    CGridView* owner_;
};
class ProductThumbnailDelegate final : public QStyledItemDelegate {
public:
    ProductThumbnailDelegate(QAbstractItemDelegate* Base, CGridView* Owner, QObject* Parent) : QStyledItemDelegate(Parent), base_(Base), owner_(Owner) {}
    QSize sizeHint(const QStyleOptionViewItem& Option, const QModelIndex& Index) const override { return base_->sizeHint(Option, Index); }
    void paint(QPainter* Painter, const QStyleOptionViewItem& Option, const QModelIndex& Index) const override {
        base_->paint(Painter, Option, Index);
        const auto Effects = Index.data(CFSModel::FileEffectsRole).toJsonObject();
        hc::ui::PaintFileRuleBorder(Painter, Option.rect, Index.data(Qt::ForegroundRole).value<QColor>(), Effects);
        hc::ui::PaintFileRulePending(Painter, Option.rect, Effects);
        owner_->RecordStartupPaint(owner_->Thumbnails()->SourceIndex(Index.row()),7);
    }
    bool editorEvent(QEvent* Event, QAbstractItemModel* Model, const QStyleOptionViewItem& Option, const QModelIndex& Index) override {
        return base_->editorEvent(Event, Model, Option, Index);
    }
private:
    QPointer<QAbstractItemDelegate> base_; CGridView* owner_;
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
    grid_->setAcceptDrops(true);
    grid_->viewport()->setAcceptDrops(true);
    // 재생 중 행의 막대 애니메이션. 보이는 재생 행의 이름 칸 하나만 다시 그린다.
    nowPlayingTimer_.setInterval(100);
    connect(&nowPlayingTimer_, &QTimer::timeout, this, [this] {
        if (!NowPlayingAnimating() || IsThumbnailMode() || IsTwoLine()) return;
        for (const auto& Hit : grid_->hitInfoAll())
            if (Hit.info() == Qtitan::GridHitInfo::Cell && Hit.modelIndex().column() == 0 && Hit.modelIndex().data(CFSModel::NowPlayingRole).toInt() == 1) {
                grid_->viewport()->update(Hit.rect()); break;
            }
    });
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
    BaseOpts.setModelItemsDragEnabled( false );
    BaseOpts.setRowStyle( Qtitan::GridViewOptions::RowStyleNormal );
    BaseOpts.setSelectedDecorationOpacity( 0.2 );
    BaseOpts.setSelectionPolicy( Qtitan::GridViewOptions::IgnoreSelection );
    BaseOpts.setFieldChooserEnabled( false );
    BaseOpts.setRubberBandSelection( false );
    BaseOpts.setTransparentBackground( true );
    // Theme tokens until the color scheme arrives; the old black · orange · navy showed before the first scheme.
    const auto& InitialColors = fm::style::ThemeManager::instance().colors();
    BaseOpts.setBackgroundColor( InitialColors[ fm::style::Token::Surface ] );
    BaseOpts.setSelectedBackgroundColor( InitialColors[ fm::style::Token::Sel ] );
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
    BaseOpts.setRowFocusColor( InitialColors[ fm::style::Token::InvCur ] );
    
    BaseOpts.setDragEnabled( false );
    BaseOpts.setDropEnabled( true );
    BaseOpts.setFieldChooserEnabled( false );
    
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
    columnStatus_ = new QLabel(this); columnStatus_->setObjectName("hc.columns.queryStatus"); columnStatus_->setMinimumWidth(0); columnStatus_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* Stop = new QPushButton(tr("컬럼 조회 중지"), this); Stop->setObjectName("hc.columns.stop");
    auto* Resume = new QPushButton(tr("다시 조회"), this); Resume->setObjectName("hc.columns.resume");
    QueryBar->addWidget(columnStatus_, 1); QueryBar->addWidget(Stop); QueryBar->addWidget(Resume);
    // Progress overlays the header without participating in the layout or moving rows.
    auto* QueryWidget = new QWidget(grid_); QueryWidget->setObjectName("hc.columns.queryOverlay");
    QueryBar->setContentsMargins(6, 2, 6, 2);
    QueryWidget->setAutoFillBackground(true); QueryWidget->setLayout(QueryBar); QueryWidget->hide();
    columnStatusDelay_.setSingleShot(true); columnStatusDelay_.setInterval(ColumnStatusDelayMs);
    connect(&columnStatusDelay_, &QTimer::timeout, this, [this] {
        if (columnStatusText_.isEmpty()) return;
        // While a key is held, results wait on purpose (InputGate); that is not a slow check.
        if (hc::ui::InputGate::Busy()) { columnStatusDelay_.start(); return; }
        showColumnStatus();
    });
    presentationStack_ = new QStackedWidget(this);
    presentationStack_->addWidget(grid_);
    QueryWidget->setParent(presentationStack_); presentationStack_->installEventFilter(this);
    connect(presentationStack_, &QStackedWidget::currentChanged, this, [this, QueryWidget] { positionColumnStatus(); QueryWidget->raise(); });
    vbox_->addWidget(presentationStack_, 1);
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
    iconViewportTimer_.setSingleShot(true); iconViewportTimer_.setInterval(0);
    connect(&iconViewportTimer_, &QTimer::timeout, this, &CGridView::updateIconViewport);
    connect(view_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { requestIconViewport(); });
    connect(view_->horizontalScrollBar(), &QScrollBar::valueChanged, this, [this] { requestIconViewport(); });
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

void CGridView::ensureThumbnails()
{
    if (thumbnails_) return;
    hc::startup::Span Trace("view.thumbnails");
    thumbnails_ = new fm::filelist::ThumbnailView(this); thumbnails_->setObjectName("hc.thumbnails");
    thumbnails_->listView()->setObjectName("hc.thumbnailTiles"); thumbnails_->listView()->setModelColumn(0);
    thumbnailModel_ = new ThumbnailModel(this); thumbnails_->setModel(thumbnailModel_);
    auto* Tiles = thumbnails_->listView(); Tiles->setItemDelegate(new ProductThumbnailDelegate(Tiles->itemDelegate(), this, Tiles));
    thumbnails_->listView()->installEventFilter(this);
    thumbnails_->listView()->viewport()->installEventFilter(this);
    thumbnails_->listView()->setDragEnabled(false);
    thumbnails_->listView()->setAcceptDrops(true);
    thumbnails_->listView()->viewport()->setAcceptDrops(true);
    presentationStack_->addWidget(thumbnails_);
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
        if (suppressMouseContextMenu_) return;
        const auto Tile = thumbnails_->listView()->indexAt(Point);
        if (Tile.isValid()) thumbnails_->setCursorRow(Tile.row());
        ShowShellMenu(Tile.isValid() ? thumbnailModel_->SourceIndex(Tile.row()) : QModelIndex{}, true);
    });
    thumbnails_->setFont(thumbnailFont_);
    thumbnails_->listView()->setPalette(QApplication::palette());
    thumbnails_->setPaneActive(panelActive_);
    thumbnailModel_->Bind(proxy_ ? static_cast<QAbstractItemModel*>(proxy_) : model_);
    if (thumbnailModel_) thumbnailModel_->SetEnabled(false);
}
ThumbnailModel* CGridView::Thumbnails() const
{
    const_cast<CGridView*>(this)->ensureThumbnails(); return thumbnailModel_;
}

CGridView::~CGridView()
{
    CancelQuickFind();
    pendingSelectionKeys_.clear(); selectionHeldKey_ = 0;
    if (auto* Native = qobject_cast<CFSModel*>(model_)) Native->SetSelectionInputActive(false);
    presentationTimer_.stop();
    automaticColumnsStop_.request_stop();
    if (thumbnailModel_) thumbnailModel_->SetEnabled(false);
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

bool CGridView::IsThumbnailMode() const { return thumbnails_ && presentationStack_ && presentationStack_->currentWidget() == thumbnails_; }
void CGridView::SetNowPlayingAnimated(bool On)
{
    // Windows의 "애니메이션 효과"는 따르지 않는다. 원격 데스크톱 세션은 사용자 선택과 무관하게 이를 꺼서
    // 시안 B안이 정지 막대로 보였다. 움직임을 원하지 않으면 오디오 설정의 확인란으로 끈다(On=false).
    if (On) { if (!nowPlayingTimer_.isActive()) nowPlayingTimer_.start(); }
    else if (nowPlayingTimer_.isActive()) { nowPlayingTimer_.stop(); grid_->viewport()->update(); }
}
bool CGridView::NowPlayingAnimating() const
{
    // 창이 비활성·최소화이면 정지된 막대로 그린다.
    const auto* Window = window();
    return nowPlayingTimer_.isActive() && isVisible() && Window && Window->isActiveWindow() && !Window->isMinimized();
}
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
void CGridView::ClearThumbnailCache() { if (thumbnailModel_) thumbnailModel_->ClearCache(); }
void CGridView::hideEvent(QHideEvent* Event)
{
    CancelRename();
    if (thumbnailModel_) thumbnailModel_->SetEnabled(false);
    if (auto* Model = qobject_cast<CFSModel*>(model_)) Model->SetIconViewport({}, false);
    CViewT::hideEvent(Event);
}
void CGridView::showEvent(QShowEvent* Event)
{
    CViewT::showEvent(Event); requestPresentation(); requestIconViewport();
}
void CGridView::applyThumbnailPresentation()
{
    const auto Settings = hc::ui::ReadThumbnailSettings(hc::settings::AppSettings().Read(hc::settings::Section::General).toObject()["thumbnails"].toObject());
    const bool Automatic = !everythingPresentation_ && ListMode() == "auto" && Settings.autoSwitch_;
    if (!thumbnails_ && (everythingPresentation_ || (ListMode() != "thumbnails" && !Automatic))) return;
    ensureThumbnails(); thumbnailModel_->Apply(Settings);
    if (ListMode() == "thumbnails" || Automatic) thumbnailModel_->Rebuild();
    const auto Percent = Automatic ? thumbnailModel_->TargetPercent() : 0.0;
    const bool Show = !everythingPresentation_ && (ListMode() == "thumbnails" || (Automatic && Percent >= Settings.autoPercent_));
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
    if (everythingPresentation_) return "two-line";
    return listModeOverride_.isEmpty() ? hc::ui::ReadFilePanelSettings(
        hc::settings::AppSettings().Read(hc::settings::Section::General).toObject().value("filePanel").toObject()).mode_ : listModeOverride_;
}
bool CGridView::SetListMode(const QString& Mode)
{
    if (everythingPresentation_) return Mode == "two-line";
    if (!hc::ui::ListModes.contains(Mode) || !columnAdapter_->Writable() || !unsupportedPresentation_.isEmpty()) return false;
    listModeOverride_ = Mode == "inherit" ? QString{} : Mode;
    nameBelowOverride_.reset(); autoInitialized_ = false;
    applyPresentation(); emit sigListPresentationChanged(); emit sigViewSettingsChanged();
    return true;
}
void CGridView::SetEverythingPresentation(bool Enabled)
{
    if (everythingPresentation_ == Enabled) return;
    everythingPresentation_ = Enabled; requestPresentation(); emit sigListPresentationChanged();
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
void CGridView::requestIconViewport()
{
    if (!iconViewportTimer_.isActive()) iconViewportTimer_.start();
}
QModelIndexList CGridView::VisibleSourceRows(int Overscan) const
{
    QModelIndexList Entries;if(!isVisible()||!view_||!model_)return Entries;
    if(IsThumbnailMode()){
        auto* List=thumbnails_->listView();const auto Area=List->viewport()->rect();
        int Low = 0, High = thumbnailModel_->rowCount();
        while (Low < High) {
            const int Middle = (Low + High) / 2;
            if (List->visualRect(thumbnailModel_->index(Middle, 0)).bottom() < Area.top()) Low = Middle + 1;
            else High = Middle;
        }
        for(int Row=Low;Row<thumbnailModel_->rowCount();++Row){
            const auto Rect = List->visualRect(thumbnailModel_->index(Row, 0));
            if (Rect.top() > Area.bottom()) break;
            if(!Rect.intersects(Area))continue;
            auto Index=thumbnailModel_->SourceIndex(Row);if(proxy_&&Index.model()==proxy_)Index=proxy_->mapToSource(Index);
            if(Index.isValid())Entries.append(Index);
        }
        return Entries;
    }
    if (!Overscan) {
        // Group headers and pinned rows have separate layout coordinates.
        // Read the actual cell rectangles instead of treating all rows as
        // one continuous scroll range.
        const auto Area = grid_->viewport()->rect();
        for (const auto& Hit : grid_->hitInfoAll()) {
            if (Hit.info() != Qtitan::GridHitInfo::Cell || !Hit.rect().intersects(Area)
                || Hit.row().type() != Qtitan::GridRow::DataRow) continue;
            auto Index = Hit.row().modelIndex(0);
            if (proxy_) Index = proxy_->mapToSource(Index);
            if (Index.isValid() && !Entries.contains(Index)) Entries.append(Index);
        }
        return Entries;
    }
    auto* Layout=static_cast<Qtitan::GridViewBase*>(view_)->layout();const int Count=view_->getRowCount();if(!Count)return Entries;
    const int Scroll=view_->verticalScrollBar()->value(),Height=std::max(1,Layout->rowsPanel()->boundedRect().height());
    const auto Position=[&](int Y){int Low=0,High=Count;while(Low+1<High){int Mid=(Low+High)/2;if(Layout->getRowScrollPos(Mid)<=Y)Low=Mid;else High=Mid;}return Low;};
    const int First=Position(std::max(0,Scroll-Overscan*Height)),Last=Position(Scroll+(1+Overscan)*Height);
    for(int At=First;At<=Last;++At){if(!Overscan&&Layout->getRowScrollPos(At)>=Scroll+Height)continue;const auto Row=view_->getRow(At);if(!Row.isValid()||Row.type()!=Qtitan::GridRow::DataRow)continue;
        auto Index=Row.modelIndex(0);if(proxy_)Index=proxy_->mapToSource(Index);if(Index.isValid())Entries.append(Index);}
    return Entries;
}
QModelIndexList CGridView::AheadSourceRows(int Screens) const
{
    // Rows partly visible at the bottom edge are returned too; callers treat the visible set first.
    QModelIndexList Entries; if (!isVisible() || !view_ || !model_ || Screens <= 0) return Entries;
    const auto Append = [&](QModelIndex Index) {
        if (proxy_ && Index.model() == proxy_) Index = proxy_->mapToSource(Index);
        if (Index.isValid()) Entries.append(Index);
    };
    if (IsThumbnailMode()) {
        auto* List = thumbnails_->listView(); const auto Area = List->viewport()->rect();
        const int Count = thumbnailModel_->rowCount(), Limit = Area.bottom() + Screens * std::max(1, Area.height());
        int Low = 0, High = Count;
        while (Low < High) {
            const int Middle = (Low + High) / 2;
            if (List->visualRect(thumbnailModel_->index(Middle, 0)).bottom() <= Area.bottom()) Low = Middle + 1;
            else High = Middle;
        }
        for (int Row = Low; Row < Count && Entries.size() < 1024; ++Row) {
            if (List->visualRect(thumbnailModel_->index(Row, 0)).top() > Limit) break;
            Append(thumbnailModel_->SourceIndex(Row));
        }
        return Entries;
    }
    auto* Layout = static_cast<Qtitan::GridViewBase*>(view_)->layout(); const int Count = view_->getRowCount(); if (!Count) return Entries;
    const int Scroll = view_->verticalScrollBar()->value(), Height = std::max(1, Layout->rowsPanel()->boundedRect().height());
    const auto Position = [&](int Y) { int Low = 0, High = Count; while (Low + 1 < High) { const int Mid = (Low + High) / 2; if (Layout->getRowScrollPos(Mid) <= Y) Low = Mid; else High = Mid; } return Low; };
    const int Last = Position(Scroll + (1 + Screens) * Height);
    for (int At = Position(Scroll + Height); At <= Last && Entries.size() < 1024; ++At) {
        const auto Row = view_->getRow(At); if (!Row.isValid() || Row.type() != Qtitan::GridRow::DataRow) continue;
        Append(Row.modelIndex(0));
    }
    return Entries;
}
void CGridView::updateIconViewport()
{
    auto* Model = qobject_cast<CFSModel*>(model_); if (!Model || !view_) return;
    // Design 6.3: slow rule values are asked for up to two screens below the viewport.
    Model->SetFileColorViewport(VisibleSourceRows(), isVisible() && panelActive_, Model->HasSlowFileRuleFields() ? AheadSourceRows(2) : QModelIndexList{});
    if (!isVisible() || IsThumbnailMode()) { Model->SetIconViewport({}, false); return; }
    auto* Layout = static_cast<Qtitan::GridViewBase*>(view_)->layout(); const int Count = view_->getRowCount();
    if (!Count) { Model->SetIconViewport({}, true); return; }
    const int Scroll = view_->verticalScrollBar()->value();
    const int Height = std::max(1, grid_->viewport()->height());
    const auto Position = [&](int Y) {
        int Low = 0, High = Count;
        while (Low + 1 < High) { const int Middle = (Low + High) / 2; if (Layout->getRowScrollPos(Middle) <= Y) Low = Middle; else High = Middle; }
        return Low;
    };
    const int First = Position(Scroll), Last = Position(Scroll + Height);
    QModelIndexList Entries;
    const auto Append = [&](int Begin, int End) {
        for (int At = Begin; At <= End && Entries.size() < 1024; ++At) {
            const auto Row = view_->getRow(At); if (!Row.isValid() || Row.type() != Qtitan::GridRow::DataRow) continue;
            auto Index = Row.modelIndex(0); if (proxy_) Index = proxy_->mapToSource(Index);
            if (Index.isValid()) Entries.append(Index);
        }
    };
    Append(First, Last); Append(Last + 1, std::min(Count - 1, Position(Scroll + 2 * Height)));
    Append(Position(std::max(0, Scroll - Height)), First - 1);
    Model->SetIconViewport(Entries, true);
}
void CGridView::RecordStartupPaint(const QModelIndex& Input,int Part)
{
    if(!hc::startup::Enabled()||hc::startup::Data().done_)return;
    auto* Model=qobject_cast<CFSModel*>(model_);if(!Model)return;
    auto Index=Input;if(proxy_&&Index.model()==proxy_)Index=proxy_->mapToSource(Index);
    if(!Index.isValid()||Index.model()!=Model||!Model->AreFileColorsReady({Index}))return;
    auto& Paint=startupPaints_[Index.row()];const auto Focus=CaptureFocus();
    if(Paint.directory_!=Model->DirectoryGeneration()||Paint.rules_!=Model->FileRuleGeneration()
        ||(Paint.focus_!=Focus&&(sourceSelectionKey(Index)==Focus||sourceSelectionKey(Index)==startupInputFocus_)))Paint={};
    Paint.focus_=Focus;Paint.directory_=Model->DirectoryGeneration();Paint.rules_=Model->FileRuleGeneration();Paint.parts_|=Part;
    if(hc::startup::Detail())hc::startup::Record("row.paint",{{"row",Index.row()},{"parts",Paint.parts_},{"focus",Focus},{"foreground",Index.data(Qt::ForegroundRole).value<QColor>().name()},{"background",Index.data(Qt::BackgroundRole).value<QColor>().name()}});
}
void CGridView::StartupPaintCompleted()
{
    if(!hc::startup::Enabled()||hc::startup::Data().done_){startupTraceTimer_.stop();return;}
    auto* Model=qobject_cast<CFSModel*>(model_);const auto Focus=CaptureFocus();
    if(!Model||!isVisible()||!panelActive_||Model->IsLoading()||Model->IsPageLoading()||Model->HasPlaceholder()||Model->LastNavigationError()
        ||presentationTimer_.isActive()||groupingTimer_.isActive()||QApplication::activeModalWidget()||QApplication::activePopupWidget())return;
    const auto Rows=VisibleSourceRows();if(Rows.isEmpty()||!Model->AreFileColorsReady(Rows))return;
    for(const auto& Index:Rows){const auto Paint=startupPaints_.value(Index.row());
        if(Paint.parts_!=7||Paint.directory_!=Model->DirectoryGeneration()||Paint.rules_!=Model->FileRuleGeneration())return;
        // Only the previous and current cursor rows require repaint on navigation.
        if((sourceSelectionKey(Index)==Focus||(startupInput_&&sourceSelectionKey(Index)==startupInputFocus_)) && Paint.focus_!=Focus)return;
    }
    auto* FocusWidget=QApplication::focusWidget();
    QJsonObject Evidence{{"rows",Rows.size()},{"focus",Focus}};
    if (hc::startup::Detail()) {
        Evidence["focusClass"] = FocusWidget ? FocusWidget->metaObject()->className() : "none";
        Evidence["focusObject"] = FocusWidget ? FocusWidget->objectName() : QString{};
        Evidence["foreground"] = QString::number(quintptr(GetForegroundWindow()));
        Evidence["window"] = QString::number(quintptr(window()->winId()));
    }
    hc::startup::Record("colors.uiPaint", Evidence);
    if(GetForegroundWindow()!=reinterpret_cast<HWND>(window()->winId())||!FocusWidget||!(FocusWidget==this||isAncestorOf(FocusWidget)))return;
    if(!hc::startup::Data().ready_)hc::startup::Ready({{"focus",Focus},{"rows",Rows.size()},{"hwnd",QString::number(quintptr(window()->winId()))}});
    if(startupInput_&&Focus!=startupInputFocus_)hc::startup::Used({{"focus",Focus},{"rows",Rows.size()}});
}
void CGridView::requestPresentation()
{
    requestIconViewport();
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
    if (everythingPresentation_) Appearance.nameBelow = true;
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
    const auto NewGeometry = fm::filelist::detail::RecordGeometry::make(recordColors_.design(), Two, Appearance);
    const bool Changed = columnAdapter_->LayoutMode() != NewMode || recordGeometry_.pitch != NewGeometry.pitch ||
        recordGeometry_.nameBelow != NewGeometry.nameBelow;
    const auto Anchor = Changed ? columnAdapter_->CaptureViewport() : QtitanColumnAdapter::ViewportAnchor{};
    QScopedValueRollback Guard(applyingPresentation_, true);
    if (unsupportedPresentation_.isEmpty() && columnAdapter_->Writable() && columnAdapter_->LayoutMode() != NewMode)
        columnAdapter_->SetLayoutMode(NewMode);
    if (auto* Native = qobject_cast<CFSModel*>(model_))
        Native->SetColumnLayoutMode(columnAdapter_->Profile().mode_);
    listAppearance_ = Appearance; recordGeometry_ = NewGeometry;
    columnAdapter_->SetTextPadding(16 + 2 * recordGeometry_.sideMargin, Two ? TwoLineTextIndent() : 0);
    // Load icons at the drawn size so 48–128 px icons are not upscaled from 16 px.
    if (auto* Native = qobject_cast<CFSModel*>(model_))
        Native->SetIconPixels(qCeil(ListIconSize() * grid_->devicePixelRatioF()));
    view_->beginUpdate();
    view_->options().setRowAutoHeight(false);
    view_->options().setCellHeight(recordGeometry_.cellHeight());
    // Qtitan insets the group caption by PM_HeaderMargin on every side. The row must hold the group
    // font plus both insets, or a large list font clips the caption's lower part.
    const int GroupText = QFontMetrics(view_->options().groupRowFont(0)).height();
    const int GroupMargin = grid_->style()->pixelMetric(QStyle::PM_HeaderMargin, nullptr, grid_);
    view_->options().setGroupRowHeight(qMax(GroupText + 12, GroupText + 2 * GroupMargin + 2));
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
    if (auto* Old = qobject_cast<CFSModel*>(model_); Old && Old != Source) Old->SetIconViewport({}, false);
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
    if (model_ != Source) navigationScroll_.clear();
    if(hc::startup::Enabled()){
        connect(Source,&QAbstractItemModel::modelAboutToBeReset,this,[this]{startupPaints_.clear();});
        connect(Source,&QAbstractItemModel::dataChanged,this,[this](const QModelIndex& A,const QModelIndex& B,const QList<int>& Roles){
            if(Roles.isEmpty()||Roles.contains(Qt::ForegroundRole)||Roles.contains(Qt::BackgroundRole)||Roles.contains(Qt::FontRole)||Roles.contains(CFSModel::FileEffectsRole)||Roles.contains(CFSModel::FileColorOverrideRole)||Roles.contains(CFSModel::ClipboardCutRole))
                for(int Row=A.row();Row<=B.row();++Row)startupPaints_.remove(Row);
        });
        startupTraceTimer_.setInterval(16);connect(&startupTraceTimer_,&QTimer::timeout,this,&CGridView::StartupPaintCompleted,Qt::UniqueConnection);startupTraceTimer_.start();
    }
    model_ = Source;
    proxy_ = Proxy;
    if (thumbnailModel_) thumbnailModel_->Bind(Proxy ? static_cast<QAbstractItemModel*>(Proxy) : Source);
    // HC-QTN-005: 8.2.0 can retain selection ranges across a model reset.
    // Clear them before rows disappear: selectedRowIndexes otherwise dereferences
    // an invalid GridRow, and old row positions must not target different files.
    disconnect(selectionReset_);
    selectionReset_ = connect(Source, &QAbstractItemModel::modelAboutToBeReset,
        this, [this] {
            resetRenameClicks(); groupingReset_ = true;
            resetSelection_ = CaptureSelection(); resetFocus_ = CaptureFocus();
            // Prefer the next surviving entry, then walk backwards when the
            // focused entry disappears (including discontiguous deletions).
            resetFocusFallback_.clear();
            const int FocusRow = view_->focusedRowIndex();
            if (!resetFocus_.isEmpty() && !resetFocus_.startsWith("@group:")) {
                const auto Remember = [this](int At) {
                    const auto Row = view_->getRow(At);
                    if (Row.type() != Qtitan::GridRow::DataRow) return;
                    const auto Key = sourceSelectionKey(Row.modelIndex(0));
                    if (!Key.isEmpty()) resetFocusFallback_.append(Key);
                };
                for (int At = FocusRow + 1; At < view_->getRowCount(); ++At) Remember(At);
                for (int At = FocusRow - 1; At >= 0; --At) Remember(At);
            }
            resetPath_ = model_->property("hc.snapshotLocation").toString();
            if (!resetPath_.isEmpty()) navigationScroll_.insert(resetPath_, new QJsonObject(CaptureScrollState()));
            model_->setProperty("hc.selectAllEntries", false);
            QScopedValueRollback GroupGuard(groupingProjection_, true); view_->deselectAll();
        }, Qt::DirectConnection);
    disconnect(selectionRestore_);
    selectionRestore_ = connect(Source, &CFsModelT::sigRefreshCompleted, this, [this] {
        const auto Path = model_->property("hc.snapshotLocation").toString();
        const auto* SavedScroll = !resetPath_.isEmpty() && resetPath_ != Path ? navigationScroll_.object(Path) : nullptr;
        const auto ReturnScroll = SavedScroll ? *SavedScroll : QJsonObject{};
        bool ParentFocused = false;
        // Focusing the parent entry can scroll it to the viewport edge. Apply
        // the saved anchor afterwards so returning preserves its screen position.
        const auto RestoreScroll = qScopeGuard([this, ReturnScroll, &ParentFocused] { if (ParentFocused) RestoreScrollState(ReturnScroll); });
        if (!resetPath_.isEmpty() && resetPath_ == model_->property("hc.snapshotLocation").toString()) {
            auto Focus = resetFocus_;
            if (!Focus.isEmpty() && !Focus.startsWith("@group:")) {
                QSet<QString> Present;
                for (const auto& Index : SelectionCandidates()) Present.insert(sourceSelectionKey(Index));
                if (!Present.contains(Focus)) {
                    Focus.clear();
                    for (const auto& Key : resetFocusFallback_) if (Present.contains(Key)) { Focus = Key; break; }
                }
            }
            RestoreSelection(resetSelection_, Focus);
            if (!Focus.isEmpty() && Focus != resetFocus_) {
                const auto Row = view_->focusedRow();
                bool Visible = false;
                for (const auto& Hit : grid_->hitInfoAll())
                    if (Hit.info() == Qtitan::GridHitInfo::Cell && Hit.row().rowIndex() == Row.rowIndex()
                        && Hit.rect().top() >= 0 && Hit.rect().bottom() < grid_->viewport()->height()) { Visible = true; break; }
                if (Row.isValid() && !Visible) view_->scrollToIndex(Row.modelIndex(0), false);
            }
        }
        // A model reset moves the cursor to the first row, but Qtitan keeps
        // the old pixel scroll offset. Only a different published location
        // starts at the top; refreshes retain their viewport and parent
        // navigation restores its saved anchor below.
        const auto NativeModel = qobject_cast<CFSModel*>(model_);
        if (!resetPath_.isEmpty() && !Path.isEmpty() && Path != resetPath_ && (!NativeModel || !NativeModel->LastNavigationError())) {
            view_->verticalScrollBar()->setValue(0);
        }
        resetPath_.clear(); resetSelection_.clear(); resetFocus_.clear(); resetFocusFallback_.clear();
        const auto Previous = model_->property("hc.parentFocusName").toString();
        const auto PreviousRoot = model_->property("hc.parentFocusPath").toString();
        const bool ParentNavigation = model_->property("hc.parentNavigation").toBool();
        model_->setProperty("hc.parentFocusPath", QVariant{});
        model_->setProperty("hc.parentFocusName", QVariant{}); model_->setProperty("hc.parentNavigation", false);
        const auto Native = qobject_cast<CFSModel*>(model_);
        if (!ParentNavigation || (Native && Native->LastNavigationError())) return;
        if (view_->getRowCount()) view_->setFocusedRowIndex(0);
        if (!hc::settings::AppSettings().Read(hc::settings::Section::General).toObject()["focusPreviousDirectory"].toBool(hc::defaults::DefaultValue("general/focusPreviousDirectory").toBool())) return;
        for (int Index = 0; (!Previous.isEmpty() || !PreviousRoot.isEmpty()) && Index < model_->rowCount(); ++Index) {
            const auto Source = model_->index(Index, 0);
            if (!PreviousRoot.isEmpty()) {
                // Shell drive labels are localized; match their filesystem identity.
                const auto Item = model_->GetFileInfo(Source);
                QString Path = model_->GetName(Source);
                if (Item && Item->storageEntry_) {
                    const auto It = Item->storageEntry_->hints_.find("filesystemPath");
                    if (It != Item->storageEntry_->hints_.end()) Path = QString::fromStdString(It->second);
                }
                if (QDir::cleanPath(QDir::fromNativeSeparators(Path)).compare(
                    QDir::cleanPath(QDir::fromNativeSeparators(PreviousRoot)), Qt::CaseInsensitive) != 0) continue;
            } else if (model_->GetName(Source) != Previous) continue;
            const auto Visible = proxy_ ? proxy_->mapFromSource(Source) : Source;
            if (!Visible.isValid()) break;
            const auto Row = view_->getRow(Visible);
            if (Row.isValid()) { view_->setFocusedRowIndex(Row.rowIndex()); ParentFocused = true; }
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
        connect(Native, &CFSModel::ColumnQueryStatus, this, [this](const QString& Text) {
            // Checks that finish within a moment never flash the overlay on folder entry;
            // a visible overlay follows the text at once and hides as soon as nothing is left.
            columnStatusText_ = Text;
            if (Text.isEmpty()) { columnStatusDelay_.stop(); columnStatus_->parentWidget()->setVisible(false); return; }
            if (columnStatus_->parentWidget()->isVisible()) showColumnStatus();
            else if (!columnStatusDelay_.isActive()) columnStatusDelay_.start();
        });
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
        connect(Source, &QAbstractItemModel::layoutChanged, this, [this] { startupPaints_.clear(); requestIconViewport(); });
        connect(Source, &QAbstractItemModel::modelReset, this, Schedule);
        connect(Source, &QAbstractItemModel::modelReset, this, [this] { groupingTimer_.start(0); });
        connect(Source, &QAbstractItemModel::rowsInserted, this, Schedule);
        connect(Source, &QAbstractItemModel::dataChanged, this, [this](const QModelIndex&, const QModelIndex&, const QList<int>& Roles) {
            // Icons and colors repaint cells; they do not change layout or text widths.
            if (Roles.isEmpty() || Roles.contains(Qt::DisplayRole) || Roles.contains(Qt::EditRole)) requestPresentation();
        });
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
              GridColumn->setProperty("hc.columnId", Column.definition_.id_);
              GridColumn->setProperty("hc.everything.path", Column.definition_.fieldId_ == "file.parentPath");
            GridColumn->dataBinding()->setSortRole(static_cast<Qt::ItemDataRole>(CFSModel::GridSortRole));
            if (Column.definition_.id_ == Columns->profile_.primaryNameId_)
            {
                GridColumn->setSortLocaleAware( true );
                
            }
            GridColumn->setFilterButtonVisible( false );
            GridColumn->setEditorType(Qtitan::GridEditor::DelegateAdapter);
            static_cast<Qtitan::GridDelegateAdapterEditorRepository*>(GridColumn->editorRepository())
                ->setDelegate(new ProductCellDelegate(this, GridColumn->editorRepository()));
            // setEditorType replaces the repository and restores its default triggers.
            // File renames use the product editor; model cells are display-only.
            GridColumn->editorRepository()->setEditorActivationPolicy(GridEditor::ActivationPolicy());
            GridColumn->editorRepository()->setEditable(false);
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
        Column->setSortCaseSensitivity(General["sortCaseSensitive"].toBool(hc::defaults::DefaultValue("general/sortCaseSensitive").toBool()));
        Column->setSortLocaleAware(General["sortLocaleAware"].toBool(hc::defaults::DefaultValue("general/sortLocaleAware").toBool()));
        const auto Order = Column->sortOrder();
        Column->setSortOrder(Order, General["sortNumericAware"].toBool(hc::defaults::DefaultValue("general/sortNumericAware").toBool()));
    }
    view_->modelController()->refresh();
    view_->endUpdate();
    RestoreSelection(Selection, Focus);
}

void CGridView::ApplyColorScheme( const TyColorScheme& ColorScheme )
{
    startupPaints_.clear();
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
    thumbnailFont_ = ColorScheme.FileList_Font;
    if (thumbnails_) { thumbnails_->setFont(thumbnailFont_); thumbnails_->listView()->setPalette(QApplication::palette()); }
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
    if (panelActive_ != Active) startupPaints_.clear();
    panelActive_ = Active;
    if (thumbnails_) thumbnails_->setPaneActive(Active);
    grid_->viewport()->update(); requestIconViewport();
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
    if (!Native || !Native->CanUseSnapshot() || !Native->ProviderEnabled() || IsThumbnailMode()) return false;
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
    // scrollToIndex(false) bottom-aligns even an already visible file.
    // Reveal the name only when it is clipped or outside the current viewport.
    const auto RowsRect = BaseView()->layout()->rowsPanel()->boundedRect();
    bool NameVisible = false;
    for (const auto& Hit : grid_->hitInfoAll()) {
        if (Hit.info() == Qtitan::GridHitInfo::Cell && Hit.row().rowIndex() == Row.rowIndex() && Hit.modelIndex().column() == 0
            && Hit.rect().top() >= RowsRect.top() && Hit.rect().bottom() <= RowsRect.bottom()) { NameVisible = true; break; }
    }
    if (!NameVisible) view_->scrollToIndex(Visible, false);
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
        auto Rect = ProductNameTextRect(this, Hit.rect(), ProductHasIcon(Visible.data(Qt::DecorationRole))).adjusted(0, 1, 0, -1);
        if (IsTwoLine()) Rect.setHeight(qMin(Rect.height(), renameEditor_->sizeHint().height()));
        Rect = Rect.intersected(grid_->viewport()->rect());
        if (!Rect.isValid()) { CancelRename(); return; }
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
        if (Row.isValid()) view_->setFocusedRowIndex(Row.rowIndex());
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

bool CGridView::ApplyColumnProfile(const hc::columns::ColumnProfile& Profile, ApplyReason Reason)
{
    if (!model_ || !hc::columns::IsSupportedProfile(Profile) || (!restoringPresentation_ && columnAdapter_ && !columnAdapter_->Writable())) return false;
    const auto PreviousProfile = ColumnProfile();
    const auto Selected = CaptureSelection(); const auto Focus = CaptureFocus();
    if (Reason == ApplyReason::Sync) {
        automaticColumnsStop_.request_stop(); ++automaticColumnsGeneration_;
        if (automaticColumnsBase_ && automaticColumnsBase_->id_ == Profile.id_) automaticColumnsBase_ = Profile;
    }
    if (!restoringPresentation_ && !applyingAutoColumns_ && Reason != ApplyReason::Sync) {
        manualColumns_ = true; automaticColumnsBase_.reset(); automaticColumnsStop_.request_stop(); ++automaticColumnsGeneration_;
    }
    if (!restoringPresentation_ && !everythingPresentation_) {
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
    if (!restoringPresentation_) {
        if (PreviousProfile.id_ == Profile.id_ && PreviousProfile.grouping_ != Profile.grouping_) {
            groupingBySet_.remove(Profile.id_); groupingBySetOrder_.removeAll(Profile.id_);
        }
        applySetGrouping(Profile);
    }
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
    QJsonArray GroupMemory;
    for (const auto& Id : groupingBySetOrder_) if (groupingBySet_.contains(Id) && GroupMemory.size() < 16)
        GroupMemory.append(QJsonObject{{"set", hc::columns::IdText(Id)}, {"grouping", hc::columns::EncodeGrouping(groupingBySet_[Id].isEmpty() ? hc::columns::GroupingState{} : groupingBySet_[Id].front())}, {"levels", hc::columns::EncodeGroupingLevels(groupingBySet_[Id])}});
    return {{"groupingLevelsV1", hc::columns::EncodeGroupingLevels(groupingLevels_)}, {"groupingBySetV1", GroupMemory}, {"groupingV1", unsupportedGrouping_.isEmpty() ? hc::columns::EncodeGrouping(grouping_) : unsupportedGrouping_}, {"listPresentation", capturePresentation()}, {"mode", "details"}, {hc::columns::WorkspaceKey, Layout}, {"columnOrderV2", Columns},
        {"hiddenSystem", Proxy && Proxy->GetHiddenSystem()}, {"search", Proxy ? Proxy->Search() : QString{}}, {"searchMatch", Proxy ? Proxy->SearchMatch() : QString{}}};
}

void CGridView::RestoreWorkspaceView(const QJsonObject& View)
{
    QScopedValueRollback RestoreGuard(restoringPresentation_, true);
    const auto SavedGrouping = View.value("groupingV1").toObject();
    const auto DecodedGrouping = hc::columns::DecodeGrouping(SavedGrouping);
    if (DecodedGrouping) { grouping_ = *DecodedGrouping; unsupportedGrouping_ = {}; }
    else { grouping_ = {}; unsupportedGrouping_ = SavedGrouping; }
    groupingLevels_ = grouping_.enabled_ ? QVector<hc::columns::GroupingState>{grouping_} : QVector<hc::columns::GroupingState>{};
    if (View.contains("groupingLevelsV1")) if (const auto Levels = hc::columns::DecodeGroupingLevels(View["groupingLevelsV1"])) {
        groupingLevels_ = *Levels; grouping_ = Levels->isEmpty() ? hc::columns::GroupingState{} : Levels->front();
    }
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
    groupingBySet_.clear(); groupingBySetOrder_.clear();
    for (const auto& Value : View["groupingBySetV1"].toArray()) {
        const auto Item = Value.toObject(); const QUuid Id(Item["set"].toString());
        const auto Saved = hc::columns::DecodeGrouping(Item["grouping"].toObject());
        if (Id.isNull() || !Item["grouping"].isObject() || !Saved || groupingBySet_.contains(Id) || groupingBySetOrder_.size() >= 16) continue;
        auto Levels = Saved->enabled_ ? QVector<hc::columns::GroupingState>{*Saved} : QVector<hc::columns::GroupingState>{};
        if (Item.contains("levels")) { const auto Decoded = hc::columns::DecodeGroupingLevels(Item["levels"]); if (!Decoded) continue; Levels = *Decoded; }
        groupingBySet_[Id] = Levels; groupingBySetOrder_.append(Id);
    }
    if (!View.contains("groupingBySetV1") && View.contains("groupingV1") && DecodedGrouping) rememberGrouping();
    const auto Profile = ColumnProfile();
    if ((Layout.isEmpty() || View.contains("groupingBySetV1")) && DecodedGrouping) applySetGrouping(Profile);
    else if (Profile.grouping_ && hc::columns::ValidateGrouping(Profile).isEmpty()) {
        expandPolicy_ = Profile.grouping_->expand_; hideGroupedColumn_ = Profile.grouping_->hideGroupedColumn_;
    }
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
                Column->setSortOrder(static_cast<Qtitan::SortOrder>(Sort), hc::settings::AppSettings().Read(hc::settings::Section::General).toObject()["sortNumericAware"].toBool(hc::defaults::DefaultValue("general/sortNumericAware").toBool()));
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
    if (Row.type() == Qtitan::GridRow::GroupRow) return "@group:" + hc::columns::NativeGroupKey(view_->modelController(), Row.modelGroup());
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
    State["anchor"] = Group ? hc::columns::NativeGroupKey(view_->modelController(), Row.modelGroup()) : sourceSelectionKey(Row.modelIndex(0));
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
        for (const auto& Group : view_->modelController()->groupRows()) if (hc::columns::NativeGroupKey(view_->modelController(), &Group) == Anchor) { Position = Group.rowIndex; break; }
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
    // A saved file index can resolve to its group header before the queued
    // grouping projection expands the rows. Finish that projection first.
    if (groupingTimer_.isActive() && !groupingProjection_ && !groupingUpdateDepth_) applyGrouping();
    const QSet<QString> Keys(Selection.begin(), Selection.end());
    const auto Previous = groupedSelection_;
    QScopedValueRollback Guard(groupingProjection_, true);
    view_->beginUpdate(); view_->deselectAll();
    if (GroupingActive()) groupedSelection_.clear();
    int FocusRow = -1; QModelIndex FocusIndex;
    QVector<int> SelectedRows;
    for (const auto& Source : SelectionCandidates()) {
        const auto Key = sourceSelectionKey(Source);
        const bool Selected = !Key.isEmpty() && Keys.contains(Key);
        if (!Selected && Key != Focus) continue;
        const auto Visible = proxy_ ? proxy_->mapFromSource(Source) : Source;
        if (Selected && GroupingActive()) groupedSelection_.insert(Key);
        const auto Row = view_->getRow(Visible); if (!Row.isValid()) continue;
        if (Selected && Row.type() == Qtitan::GridRow::DataRow) SelectedRows.append(Row.rowIndex());
        if (!Key.isEmpty() && Key == Focus) { FocusRow = Row.rowIndex(); FocusIndex = Visible; }
    }
    if (Focus.startsWith("@group:")) for (const auto& Group : view_->modelController()->groupRows())
        if (hc::columns::NativeGroupKey(view_->modelController(), &Group) == Focus.mid(7)) { FocusRow = Group.rowIndex; break; }
    std::sort(SelectedRows.begin(), SelectedRows.end());
    const auto* First = view_->getFirstColumn(); const auto* Last = view_->getLastColumn();
    for (int At = 0; At < SelectedRows.size();) {
        const int Start = SelectedRows[At]; int End = Start;
        while (++At < SelectedRows.size() && SelectedRows[At] == End + 1) End = SelectedRows[At];
        view_->selectRange(Start, End, First ? First->index() : -1, Last ? Last->index() : -1, Qtitan::Select);
    }
    view_->endUpdate();
    if (FocusIndex.isValid()) FocusRow = view_->getRow(FocusIndex).rowIndex();
    if (FocusRow >= 0) view_->setFocusedRowIndex(FocusRow);
    if (GroupingActive() && Previous != groupedSelection_) emit sigSelectionIdentityChanged();
    selectionSummaryTimer_.start(0);
}

void CGridView::RemoveSelection(const QStringList& Selection)
{
    const QSet<QString> Keys(Selection.begin(), Selection.end());
    const auto Remove = [&Keys](QStringList& List) { List.removeIf([&Keys](const QString& Key) { return Keys.contains(Key); }); };
    // A provider refresh may already have saved the selection for restoration.
    // Remove completed entries there as well, so they cannot reappear after reset.
    Remove(resetSelection_); Remove(querySelection_);
    if (model_) model_->setProperty("hc.selectAllEntries", false);
    // Loading keeps the old rows visible until the new snapshot is published.
    // Clear those marks too, before modelAboutToBeReset captures them again.
    auto Current = CaptureSelection(); Remove(Current);
    RestoreSelection(Current, CaptureFocus());
}

void CGridView::showColumnStatus()
{
    columnStatus_->setText(columnStatusText_); columnStatus_->setToolTip(columnStatusText_); positionColumnStatus();
    columnStatus_->parentWidget()->setVisible(true); columnStatus_->parentWidget()->raise();
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
    if (IsSelected && Info && Info->Name_ != ".." && (Info->Attributes_ & FILE_ATTRIBUTE_DIRECTORY) && !Info->directorySize_.has_value()
        && hc::settings::AppSettings().CommittedObject()["options"].toObject()["folderSizeOnSpace"].toBool(hc::defaults::DefaultValue("options/folderSizeOnSpace").toBool()))
        if (auto* Native = qobject_cast<CFSModel*>(model_)) Native->RequestDirectorySizes({ModelIndex});
    
    if( IsMoveDown == true )
        view_->navigateDown( Qt::ControlModifier );

    return Info;
}

bool CGridView::invertSelectionAlongNavigation(QKeyCombination Key)
{
    const auto Modifiers = Key.keyboardModifiers() & ~Qt::KeypadModifier;
    const bool ControlBoundary = (Key.key() == Qt::Key_Home || Key.key() == Qt::Key_End)
        && Modifiers == (Qt::ShiftModifier | Qt::ControlModifier);
    if (Modifiers != Qt::ShiftModifier && !ControlBoundary) return false;
    switch (Key.key()) {
    case Qt::Key_Up: case Qt::Key_Down: case Qt::Key_Left: case Qt::Key_Right:
    case Qt::Key_Home: case Qt::Key_End: case Qt::Key_PageUp: case Qt::Key_PageDown: break;
    default: return false;
    }
    const bool Tiles = IsThumbnailMode();
    const int Before = Tiles ? thumbnails_->listView()->currentIndex().row() : view_->focusedRowIndex();
    if (Before < 0) return true;
    if (Tiles) {
        QScopedValueRollback Guard(forwardingSelectionNavigation_, true);
        QKeyEvent Move(QEvent::KeyPress, Key.key(), Qt::NoModifier);
        QApplication::sendEvent(thumbnails_->listView(), &Move);
    } else {
        switch (Key.key()) {
        case Qt::Key_Up: view_->navigateUp(Qt::ControlModifier); break;
        case Qt::Key_Down: view_->navigateDown(Qt::ControlModifier); break;
        case Qt::Key_Left: view_->navigateLeft(Qt::ControlModifier); break;
        case Qt::Key_Right: view_->navigateRight(Qt::ControlModifier); break;
        case Qt::Key_Home: view_->setFocusedRowIndex(0, Qt::ControlModifier); break;
        case Qt::Key_End: view_->setFocusedRowIndex(view_->getRowCount() - 1, Qt::ControlModifier); break;
        case Qt::Key_PageUp: view_->navigatePageUp(Qt::ControlModifier); break;
        case Qt::Key_PageDown: view_->navigatePageDown(Qt::ControlModifier); break;
        default: break;
        }
    }
    const int After = Tiles ? thumbnails_->listView()->currentIndex().row() : view_->focusedRowIndex();
    if (After < 0 || Before == After) return true;
    // Toggle departed rows only, so repeats do not toggle a shared endpoint twice.
    const int Step = After > Before ? 1 : -1;
    const auto* FirstColumn = view_->getFirstColumn();
    const auto* LastColumn = view_->getLastColumn();
    int Start = -1, Last = -1;
    const auto Flush = [&] {
        if (Start < 0) return;
        view_->selectRange(qMin(Start, Last), qMax(Start, Last),
            FirstColumn ? FirstColumn->index() : -1, LastColumn ? LastColumn->index() : -1, Qtitan::Invert);
        Start = Last = -1;
    };
    view_->beginUpdate();
    for (int I = Before; I != After; I += Step) {
        const auto Row = Tiles ? view_->getRow(thumbnailModel_->SourceIndex(I)) : view_->getRow(I);
        if (!Row.isValid() || Row.type() != Qtitan::GridRow::DataRow) { Flush(); continue; }
        const auto Index = Row.modelIndex(0);
        const auto Source = proxy_ ? proxy_->mapToSource(Index) : Index;
        const auto Info = model_->GetFileInfo(Source);
        if (!Info || Info->Name_ == "..") { Flush(); continue; }
        if (Start >= 0 && Row.rowIndex() != Last + Step) Flush();
        if (Start < 0) Start = Row.rowIndex();
        Last = Row.rowIndex();
    }
    Flush();
    view_->endUpdate();
    model_->setProperty("hc.selectAllEntries", false);
    selectionSummaryTimer_.start(0);
    return true;
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
            || (Native && !Native->CanUseSnapshot())) {
            selectionKeyTimer_.start(); return;
        }
        const auto Key = pendingSelectionKeys_.takeFirst();
        QKeyEvent Event(QEvent::KeyPress, Key.key(), Key.keyboardModifiers());
        if (!TyStCommandMgr::GetInstance()->ProcessKeyPressEvent(&Event, retrieveCursorIndex()))
            invertSelectionAlongNavigation(Key);
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

QStringList CGridView::DragPaths(const QModelIndex& Index) const
{
    auto Source = Index;
    while (const auto* Proxy = qobject_cast<const QAbstractProxyModel*>(Source.model())) Source = Proxy->mapToSource(Source);
    if (!model_ || !Source.isValid() || Source.model() != model_) return {};
    const auto Item = model_->GetFileInfo(Source);
    // Shell menus may fall back to the current directory; drags must never do so.
    if (!Item || Item->Name_ == "..") return {};
    return ShellMenuPaths(Source, IsSourceSelected(Source));
}

QString CGridView::FileDropDirectory(const QModelIndex& Index, bool SameView) const
{
    const auto* Native = qobject_cast<CFSModel*>(model_);
    if (!Native || !Native->CanUseSnapshot() || IsEditing()) return {};
    const auto Kind = Native->WorkspaceLocation().kind_;
    if (!Native->IsSearchResults() && Kind != hc::storage::LocatorKind::Local && Kind != hc::storage::LocatorKind::Smb) return {};
    auto Source = Index;
    while (const auto* Proxy = qobject_cast<const QAbstractProxyModel*>(Source.model())) Source = Proxy->mapToSource(Source);
    if (Source.isValid()) {
        if (Source.model() != model_) return {};
        const auto Item = model_->GetFileInfo(Source);
        if (Item && (Item->Attributes_ & FILE_ATTRIBUTE_DIRECTORY))
            return QDir::cleanPath(QDir::fromNativeSeparators(Native->GetFileFullPath(Source)));
    }
    // Search results have no background destination; same-view drops require a folder.
    if (SameView || Native->IsSearchResults()) return {};
    return QDir::cleanPath(QDir::fromNativeSeparators(Native->GetCurrentPathWithRoot()));
}

Qt::DropAction CGridView::ChooseFileDropAction(int Count, const QString& Destination, Qt::DropActions Allowed, const QPoint& Position)
{
    QMenu Menu(this); Menu.setObjectName("hc.fileDropMenu");
    auto* Header = new QWidget(&Menu);
    auto* Layout = new QVBoxLayout(Header); Layout->setContentsMargins(28, 8, 16, 8); Layout->setSpacing(4);
    auto* CountLabel = new QLabel(tr("%1개 항목").arg(Count), Header);
    auto* TargetLabel = new QLabel(Header); TargetLabel->setObjectName("hc.fileDropDestination");
    const auto Target = QDir::toNativeSeparators(Destination);
    // Bound long UNC paths without hiding their full value from hover/assistive tools.
    const auto Available = screen() ? screen()->availableGeometry().width() : 800;
    TargetLabel->setText(TargetLabel->fontMetrics().elidedText(tr("대상  %1").arg(Target), Qt::ElideMiddle, qMin(440, Available - 100)));
    TargetLabel->setToolTip(Target); TargetLabel->setAccessibleName(tr("대상  %1").arg(Target));
    auto Palette = TargetLabel->palette(); Palette.setColor(QPalette::WindowText, recordColors_[fm::style::Token::Fg3]); TargetLabel->setPalette(Palette);
    Layout->addWidget(CountLabel); Layout->addWidget(TargetLabel);
    auto* HeaderAction = new QWidgetAction(&Menu); HeaderAction->setDefaultWidget(Header); Menu.addAction(HeaderAction);
    Menu.addSeparator();
    const auto Add = [&](const QString& Text, fm::style::Glyph Glyph, Qt::DropAction Action) {
        auto* Item = Menu.addAction(fm::style::themedGlyphIcon(Glyph, &Menu), Text);
        Item->setData(int(Action)); Item->setEnabled(Allowed.testFlag(Action)); return Item;
    };
    auto* Copy = Add(tr("여기에 복사"), fm::style::Glyph::Copy, Qt::CopyAction);
    Add(tr("여기로 이동"), fm::style::Glyph::Move, Qt::MoveAction);
    Add(tr("여기에 바로 가기 만들기"), fm::style::Glyph::ReturnArrow, Qt::LinkAction);
    Menu.addSeparator(); auto* Cancel = Menu.addAction(tr("취소")); Cancel->setData(int(Qt::IgnoreAction));
    Menu.setDefaultAction(Copy); Menu.setActiveAction(Copy->isEnabled() ? Copy : Cancel);
    const auto* Chosen = Menu.exec(Position);
    return Chosen && Chosen->data().isValid() ? Qt::DropAction(Chosen->data().toInt()) : Qt::IgnoreAction;
}

QStringList CGridView::ShellMenuPaths(const QModelIndex& Index, bool UseSelection) const
{
    const auto* Native = qobject_cast<CFSModel*>(model_);
    if (!Native || !Native->CanUseSnapshot() || !view_) return {};
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
    // Remote/input desktops can deny GetCursorPos even while the file panel
    // receives mouse events. Keep the menu usable at the focused row instead.
    if (!AtCursor || !GetCursorPos(&Position)) {
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
    // 오디오 파일 하나면 셸 메뉴 맨 위에 "미리 듣기"를 둔다.
    ShellMenuExtras Extras;
    auto* Bridge = TyStCommandMgr::GetInstance()->Bridge();
    if (Bridge && Bridge->Audio() && Paths.size() == 1 && Source.isValid())
        if (const auto Item = model_->GetFileInfo(Source); Item && !(Item->Attributes_ & FILE_ATTRIBUTE_DIRECTORY)
            && hc::audio::IsPlayable(Item->Name_, Bridge->Audio()->Extensions())) {
            CmpPanel* Panel = nullptr;
            for (auto* Parent = parent(); Parent && !Panel; Parent = Parent->parent()) Panel = qobject_cast<CmpPanel*>(Parent);
            if (Panel) Extras.append({tr("미리 듣기"), [Bridge = QPointer<CommandUiBridge>(Bridge), Panel = QPointer<CmpPanel>(Panel), Source = QPersistentModelIndex(Source)] {
                if (Bridge && Panel && Source.isValid()) Bridge->StartAudioPreview(Panel, Source);
            }});
        }
    return OpenShellContextMenuForObjects(Paths, Position.x, Position.y, reinterpret_cast<void*>(window()->winId()), Extras);
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
    if (suppressMouseContextMenu_ || rightPressActive_) { Args->setHandled(true); return; }
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
        || !hc::settings::AppSettings().Read(hc::settings::Section::Options).toObject()["renameOnSlowClick"].toBool(hc::defaults::DefaultValue("options/renameOnSlowClick").toBool())) {
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
    if (OldRowIndex != NewRowIndex) { resetRenameClicks(); emit sigPreviewSelectionChanged(); }
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
    // Reject every native editor, including Enter with modifiers and explicit
    // showEditor calls. Only BeginRename may open the product rename editor.
    viewClickTimer_.stop(); mouseRenameIndex_ = QPersistentModelIndex{};
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

void CGridView::positionColumnStatus()
{
    if (!columnStatus_) return;
    auto* Overlay = columnStatus_->parentWidget();
    Overlay->setGeometry(0, 0, presentationStack_->width(), Overlay->sizeHint().height());
}

bool CGridView::eventFilter( QObject* Watched, QEvent* Event )
{
    if (Watched == presentationStack_ && Event->type() == QEvent::Resize) positionColumnStatus();
    if (forwardingSelectionNavigation_) return false;
    if(hc::startup::Enabled()&&Event->type()==QEvent::KeyPress){
        const auto* Key=static_cast<QKeyEvent*>(Event);
        if(hc::startup::Data().ready_&&!startupInput_&&(Key->key()==Qt::Key_Down||Key->key()==Qt::Key_Up)){
            startupInput_=true;startupInputFocus_=CaptureFocus();hc::startup::Record("input.received",{{"key",Key->key()},{"focus",startupInputFocus_}});
        }
    }
    const bool GridViewport = Watched == grid_->viewport();
    const bool TileViewport = thumbnails_ && Watched == thumbnails_->listView()->viewport();
    const bool FileSurface = GridViewport || TileViewport || Watched == grid_ || (thumbnails_ && Watched == thumbnails_->listView());
    if (FileSurface && Event->type() == QEvent::ContextMenu) {
        const auto* Context = static_cast<QContextMenuEvent*>(Event);
        if (Context->reason() == QContextMenuEvent::Keyboard) suppressMouseContextMenu_ = false;
        else if (suppressMouseContextMenu_ || rightPressActive_) { Event->accept(); return true; }
    }
    if (FileSurface && Event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(Event)->key() == Qt::Key_Escape && rightPressActive_) {
        rightPressActive_ = false; suppressMouseContextMenu_ = true; dragPaths_.clear();
    }
    if (GridViewport || TileViewport) {
        if (Event->type() == QEvent::MouseButtonPress) {
            dragPaths_.clear();
            const auto* Mouse = static_cast<QMouseEvent*>(Event);
            dragButton_ = Mouse->button();
            if (dragButton_ == Qt::RightButton) {
                rightPressGlobalPosition_ = Mouse->globalPosition();
                rightPressActive_ = true; suppressMouseContextMenu_ = false;
            }
            if ((dragButton_ == Qt::LeftButton || dragButton_ == Qt::RightButton) && !IsEditing()) {
                dragPressPosition_ = Mouse->position().toPoint();
                QModelIndex Index;
                if (TileViewport) {
                    const auto Tile = thumbnails_->listView()->indexAt(dragPressPosition_);
                    if (Tile.isValid()) Index = thumbnailModel_->SourceIndex(Tile.row());
                } else {
                    const auto Hit = grid_->hitInfo(dragPressPosition_);
                    if (Hit.info() == Qtitan::GridHitInfo::Cell) Index = Hit.modelIndex();
                }
                dragPaths_ = DragPaths(Index);
            }
        } else if (Event->type() == QEvent::MouseButtonRelease || Event->type() == QEvent::MouseButtonDblClick) {
            dragPaths_.clear();
            const auto* Mouse = static_cast<QMouseEvent*>(Event);
            if (Mouse->button() == Qt::RightButton && rightPressActive_) {
                suppressMouseContextMenu_ |= Mouse->globalPosition() != rightPressGlobalPosition_;
                rightPressActive_ = false;
            }
        } else if (Event->type() == QEvent::MouseMove) {
            const auto* Mouse = static_cast<QMouseEvent*>(Event);
            if (rightPressActive_ && Mouse->buttons().testFlag(Qt::RightButton))
                suppressMouseContextMenu_ |= Mouse->globalPosition() != rightPressGlobalPosition_;
            if (!Mouse->buttons().testFlag(dragButton_)) dragPaths_.clear();
            else if (!dragPaths_.isEmpty() && (Mouse->position().toPoint() - dragPressPosition_).manhattanLength() >= QApplication::startDragDistance()) {
                const auto Paths = std::exchange(dragPaths_, {});
                const auto Button = dragButton_;
                suppressMouseContextMenu_ = true; rightPressActive_ = false;
                plainLeftClick_ = false; resetRenameClicks();
                // End the view's pressed state before entering the native OLE drag loop.
                QMouseEvent Release(QEvent::MouseButtonRelease, Mouse->position(), Mouse->globalPosition(),
                    Button, Qt::NoButton, Mouse->modifiers());
                QCoreApplication::sendEvent(Watched, &Release);
                resetRenameClicks();
                auto* Mime = new hc::ui::FileDragMime(Paths);
                auto* Drag = new QDrag(this); Drag->setMimeData(Mime);
                Mime->setData("application/x-hypercommander-drag-button", Button == Qt::RightButton ? "right" : "left");
                Mime->setData("application/x-hypercommander-source-panel", FilePanelDragToken(this).toUtf8());
                // Explorer must copy a left drag even on the same volume.
                Drag->exec(Button == Qt::RightButton ? Qt::CopyAction | Qt::MoveAction | Qt::LinkAction : Qt::CopyAction, Qt::CopyAction);
                Drag->deleteLater();
                // The shell target performs a move; never remove source files here.
                return true;
            }
        }
    }
    if (Event->type() == QEvent::ShortcutOverride && !IsEditing()
        && (Watched == grid_ || Watched == grid_->viewport())) {
        const auto* Key = static_cast<QKeyEvent*>(Event);
        const auto Id = hc::ui::ShortcutCommand(*Key, TyStCommandMgr::GetInstance()->Retrieve());
        const auto* Bridge = TyStCommandMgr::GetInstance()->Bridge();
        if (Id == "hc.navigate.enterFile" || Id.startsWith("hc.clipboard.")
            || (Key->key() == Qt::Key_Escape && Key->modifiers() == Qt::NoModifier && Bridge
                && (Bridge->HasFileClipboard() || Bridge->HasAudioPreview()))) {
            Event->accept(); return true;
        }
    }
    if (Event->type() == QEvent::KeyRelease && FileSurface) {
        const auto* Key = static_cast<QKeyEvent*>(Event);
        if (!Key->isAutoRepeat() && Key->key() == selectionHeldKey_) finishSelectionInput();
    }
    if (Event->type() == QEvent::KeyPress && FileSurface && !IsEditing()) {
        const auto* Key = static_cast<QKeyEvent*>(Event);
        const auto Modifiers = Key->modifiers() & ~Qt::KeypadModifier;
        const bool ControlBoundary = (Key->key() == Qt::Key_Home || Key->key() == Qt::Key_End)
            && Modifiers == (Qt::ShiftModifier | Qt::ControlModifier);
        const bool ShiftNavigation = ((Modifiers == Qt::ShiftModifier)
            && (Key->key() == Qt::Key_Up || Key->key() == Qt::Key_Down || Key->key() == Qt::Key_Left || Key->key() == Qt::Key_Right
                || Key->key() == Qt::Key_Home || Key->key() == Qt::Key_End || Key->key() == Qt::Key_PageUp || Key->key() == Qt::Key_PageDown))
            || ControlBoundary;
        if (ShiftNavigation || hc::ui::CommandId(TyStCommandMgr::GetInstance()->Retrieve().value(Key->keyCombination())) == "hc.selection.invert") {
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
    if (FileSurface && Event->type() == QEvent::DragLeave) { rightDrop_ = false; Event->accept(); return true; }
    if (FileSurface && (Event->type() == QEvent::DragEnter || Event->type() == QEvent::DragMove || Event->type() == QEvent::Drop)) {
        auto* Drop = static_cast<QDropEvent*>(Event);
        if (Event->type() == QEvent::DragEnter) rightDrop_ = false;
        rightDrop_ |= Drop->buttons().testFlag(Qt::RightButton)
            || Drop->mimeData()->data("application/x-hypercommander-drag-button") == "right";
        if (rightDrop_) { suppressMouseContextMenu_ = true; rightPressActive_ = false; }
        QStringList Paths;
        for (const auto& Url : Drop->mimeData()->urls()) {
            if (!Url.isLocalFile() || Url.toLocalFile().contains(QChar::Null) || !QDir::isAbsolutePath(Url.toLocalFile())) { Drop->ignore(); return true; }
            Paths.append(QDir::cleanPath(QDir::fromNativeSeparators(Url.toLocalFile())));
        }
        Paths.removeDuplicates();
        auto Allowed = Drop->possibleActions() & (Qt::CopyAction | Qt::MoveAction | Qt::LinkAction);
        if (!rightDrop_) Allowed &= Qt::CopyAction;
        const auto Reject = [&] {
            Drop->setDropAction(Qt::IgnoreAction);
            // Keep a valid payload's entry accepted so later DragMove events
            // can reach folders, while reporting a forbidden cursor here.
            if (Event->type() == QEvent::DragEnter && !Paths.isEmpty() && Allowed) Drop->accept();
            else Drop->ignore();
        };
        QModelIndex Index;
        if (TileViewport || (thumbnails_ && Watched == thumbnails_->listView())) {
            const auto Point = thumbnails_->listView()->viewport()->mapFrom(static_cast<QWidget*>(Watched), Drop->position().toPoint());
            const auto Tile = thumbnails_->listView()->indexAt(Point);
            if (Tile.isValid()) Index = thumbnailModel_->SourceIndex(Tile.row());
        } else {
            const auto Point = grid_->viewport()->mapFrom(static_cast<QWidget*>(Watched), Drop->position().toPoint());
            const auto Hit = grid_->hitInfo(Point);
            if (Hit.info() == Qtitan::GridHitInfo::Cell) Index = Hit.modelIndex();
            else if (Hit.info() != Qtitan::GridHitInfo::Rows) { Reject(); return true; }
        }
        const auto PanelFor = [](const QWidget* Widget) {
            for (; Widget; Widget = Widget->parentWidget()) if (const auto* Panel = qobject_cast<const CmpPanel*>(Widget)) return Panel;
            return static_cast<const CmpPanel*>(nullptr);
        };
        const auto* Source = qobject_cast<const CGridView*>(Drop->source());
        const auto SourcePanel = Drop->mimeData()->data("application/x-hypercommander-source-panel");
        const bool SamePanel = (!SourcePanel.isEmpty() && SourcePanel == FilePanelDragToken(this).toUtf8())
            || (Source && (Source == this || (PanelFor(Source) && PanelFor(Source) == PanelFor(this))));
        const auto Destination = FileDropDirectory(Index, SamePanel);
        const bool Recursive = std::any_of(Paths.begin(), Paths.end(), [&](const auto& Path) {
            return Destination.compare(Path, Qt::CaseInsensitive) == 0 || Destination.startsWith(Path + '/', Qt::CaseInsensitive);
        });
        if (Paths.isEmpty() || Destination.isEmpty() || Recursive || !Allowed) { Reject(); return true; }
        auto Action = Allowed.testFlag(Qt::CopyAction) ? Qt::CopyAction : Allowed.testFlag(Qt::MoveAction) ? Qt::MoveAction : Qt::LinkAction;
        if (Event->type() == QEvent::Drop) {
            const bool Right = std::exchange(rightDrop_, false);
            QPointer<CGridView> Alive(this);
            if (Right) Action = ChooseFileDropAction(Paths.size(), Destination, Allowed, static_cast<QWidget*>(Watched)->mapToGlobal(Drop->position().toPoint()));
            if (!Alive || Action == Qt::IgnoreAction) { Drop->ignore(); return true; }
            auto* Bridge = TyStCommandMgr::GetInstance()->Bridge();
            if (!Bridge) { Drop->ignore(); return true; }
            Bridge->DropFiles(Paths, Destination, Action);
            // HC owns source removal in its asynchronous move job. The source
            // application must not delete it again when the OLE loop returns.
            if (Action == Qt::MoveAction) Action = Qt::TargetMoveAction;
        }
        Drop->setDropAction(Action); Drop->accept(); return true;
    }
    if (Watched == grid_->viewport() && (Event->type() == QEvent::Paint || Event->type() == QEvent::Resize)) {
        if (Event->type() == QEvent::Resize) startupPaints_.clear();
        requestIconViewport();
    }
    // Changing the main window frame stylesheet on maximize repolishes children,
    // which resets viewport auto-fill. Restore it after stylesheet processing.
    if (Watched == grid_ && Event->type() == QEvent::StyleChange)
        QTimer::singleShot(0, this, &CGridView::syncViewportBackground);
    // A DPI change reloads icons at the new physical size.
    if (Event->type() == QEvent::FontChange || Event->type() == QEvent::Show || Event->type() == QEvent::DevicePixelRatioChange) requestPresentation();
    if (Watched == grid_->viewport() && Event->type() == QEvent::KeyPress) resetRenameClicks();

    if( Event->type() == QEvent::KeyPress )
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
    auto* Close = new QAction(tr("미리 보기 닫기"), this);
    Close->setShortcut(QKeySequence(hc::defaults::AuxiliaryShortcut("quickView.close")));
    Close->setShortcutContext(Qt::WidgetWithChildrenShortcut); addAction(Close);
    connect(Close, &QAction::triggered, this, &CQuickView::CloseRequested);
    auto* Bar = new QToolBar(this);
    returnAction_ = Bar->addAction(tr("파일 패널로"));
    addAction(returnAction_);
    connect(returnAction_, &QAction::triggered, this, &CQuickView::ReturnFocusRequested);
    auto* Follow = Bar->addAction(tr("선택 따라가기"));
    connect(Follow, &QAction::triggered, this, [this] {
        if (surface_) surface_->ResumeFollowing(); else emit FollowSelectionRequested();
    });
    Bar->addAction(Close);
    vbox_->addWidget(Bar);
    connect(TyStCommandMgr::GetInstance(), &CCommandMgr::ShortcutsChanged, this, &CQuickView::refreshReturnKey);
    refreshReturnKey();
}

void CQuickView::refreshReturnKey()
{
    QKeyCombination Key(Qt::Key_unknown);
    const auto Values = TyStCommandMgr::GetInstance()->Retrieve();
    for (auto It = Values.cbegin(); It != Values.cend(); ++It)
        if (hc::ui::CommandId(It.value()) == "hc.viewer.returnFocus") { Key = It.key(); break; }
    const auto Sequence = Key.key() == Qt::Key_unknown ? QKeySequence{} : QKeySequence(Key);
    returnAction_->setShortcut(Sequence);
    returnAction_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    returnAction_->setToolTip(tr("파일 패널로: %1").arg(Sequence.isEmpty() ? tr("단축키 없음") : Sequence.toString(QKeySequence::NativeText)));
    if (surface_) surface_->SetReturnKey(Key);
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

void CQuickView::SetContent(QtViewerHost& Host, std::shared_ptr<hc::storage::IContentSource> Source,
    std::shared_ptr<hc::storage::IWritableStorage> Writer)
{
    if (IsPinned() || !Source) return;
    cancelProbe();
    const auto Entry = Source->Describe();
    const auto Extension = QFileInfo(QString::fromStdString(Entry.name_)).suffix().toLower().toStdString();
    const auto& Formats = hc::document::DocumentExtensions;
    if (Entry.directory_ || std::find(Formats.begin(), Formats.end(), Extension) != Formats.end()) {
        openContent(Host, std::move(Source), std::move(Writer), false); return;
    }
    SetInformation(tr("파일 형식을 확인하는 중…\n\n%1").arg(QString::fromStdString(Entry.name_)));
    const auto Generation = probeGeneration_;
    const auto Stop = probeStop_.get_token();
    auto* Watcher = new QFutureWatcher<bool>(this);
    connect(Watcher, &QFutureWatcher<bool>::finished, this,
        [this, Watcher, Generation, HostGuard = QPointer<QtViewerHost>(&Host), Source, Writer] {
        const auto TextContent = Watcher->result(); Watcher->deleteLater();
        if (Generation != probeGeneration_ || !HostGuard || IsPinned()) return;
        openContent(*HostGuard, Source, Writer, TextContent);
    });
    Watcher->setFuture(QtConcurrent::run([Source, Stop] {
        hc::storage::OperationContext Context; Context.stopToken_ = Stop;
        Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        Context.maxReadBytes_ = 64 * 1024;
        return hc::document::ProbeDocumentText(*Source, Context);
    }));
}
CQuickView::~CQuickView() { cancelProbe(); }
void CQuickView::cancelProbe()
{
    probeStop_.request_stop(); probeStop_ = std::stop_source{}; ++probeGeneration_;
}
void CQuickView::openContent(QtViewerHost& Host, std::shared_ptr<hc::storage::IContentSource> Source,
    std::shared_ptr<hc::storage::IWritableStorage> Writer, bool TextContent)
{
    if (IsPinned() || !Source) return;
    const auto Entry = Source->Describe();
    informationText_ = tr("파일 정보\n\n이름: %1\n위치: %2\n크기: %3 byte")
        .arg(QString::fromStdString(Entry.name_), QString::fromStdString(Entry.locator_.root_)).arg(Entry.size_);
    auto& Settings = hc::settings::AppSettings();
    const auto SettingsKey = QJsonDocument(QJsonObject{{"editors", Settings.Read(hc::settings::Section::Editors)},
        {"plugins", Settings.Read(hc::settings::Section::Plugins)}}).toJson(QJsonDocument::Compact);
    const auto Stamp = [](const QString& Path) { const QFileInfo File(Path); return qMakePair(File.size(), File.lastModified().toMSecsSinceEpoch()); };
    bool Changed = !handlerCatalog_ || handlerSettings_ != SettingsKey;
    for (auto It = handlerFiles_.cbegin(); !Changed && It != handlerFiles_.cend(); ++It) Changed = Stamp(It.key()) != It.value();
    if (Changed) {
        std::vector<hc::document::DocumentHandlerTarget> Plugins;
        for (const auto& Choice : QPluginHostSettings::ViewerChoices(Settings))
            Plugins.push_back(hc::document::BuildPluginViewerTarget(Choice.name_, Choice.options_, Choice.extensions_));
        handlerCatalog_ = std::make_shared<hc::document::DocumentHandlerCatalog>(hc::document::BuildDocumentHandlerCatalog(Settings, QCoreApplication::applicationDirPath(), std::move(Plugins)));
        handlerSettings_ = SettingsKey; handlerFiles_.clear();
        for (const auto& [Id, Target] : handlerCatalog_->targets_)
            for (const auto& Path : {Target.plugin_.binaryPath_, Target.plugin_.manifestPath_, Target.plugin_.executable_, Target.executable_})
                if (!Path.isEmpty()) handlerFiles_.insert(Path, Stamp(Path));
    }
    const auto& Catalog = *handlerCatalog_;
    auto Default = Catalog.defaultView_;
    const auto Override = Catalog.overrides_[QFileInfo(QString::fromStdString(Entry.name_)).suffix().toLower()].toObject()["view"].toString();
    if (!Override.isEmpty()) Default = Override.toStdString();
    hc::document::HandlerSelection Selection; Selection.names_ = {Entry.name_}; Selection.textContent_ = {TextContent};
    const auto Resolution = hc::document::DocumentHandlerResolver::Resolve(Catalog.registry_, hc::document::DocumentIntent::View, Selection, Default);
    std::vector<QtViewerSurface::PluginViewerChoice> Choices;
    for (const auto& Id : Resolution.candidates_) {
        const auto& Target = Catalog.targets_.at(Id);
        if (Target.descriptor_.kind_ != hc::document::HandlerKind::Plugin) continue;
        auto Options = Target.plugin_; Options.writer_ = Writer;
        Choices.push_back({QString::fromStdString(Id), QString::fromStdString(Target.descriptor_.name_), std::move(Options), {}});
    }
    if (Choices.empty()) { SetInformation(informationText_ + tr("\n\n지원하는 미리보기 처리기가 없습니다.")); return; }
    if (information_) information_->hide();
    if (!surface_)
    {
        surface_ = Host.CreateSurface(this); vbox_->addWidget(surface_); setFocusProxy(surface_);
        connect(surface_, &QtViewerSurface::CloseRequested, this, &CQuickView::CloseRequested);
        connect(surface_, &QtViewerSurface::ReturnFocusRequested, this, &CQuickView::ReturnFocusRequested);
        connect(surface_, &QtViewerSurface::FollowSelectionRequested, this, &CQuickView::FollowSelectionRequested);
        connect(surface_, &QtViewerSurface::Failed, this, [this](const QString& Code) {
            if (!IsPinned()) SetInformation(informationText_ + tr("\n\n미리보기를 열 수 없습니다: %1").arg(hc::ui::ErrorText(Code)));
        });
    }
    surface_->show(); setFocusProxy(surface_);
    refreshReturnKey();
    surface_->OpenPreview(std::move(Source), std::move(Choices));
}

void CQuickView::SetInformation(const QString& Information)
{
    if (IsPinned()) return;
    cancelProbe();
    if (surface_) { surface_->Close(); surface_->hide(); }
    if (!information_) {
        auto* Label = new QLabel(this);
        Label->setTextFormat(Qt::PlainText);
        Label->setWordWrap(true);
        Label->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        Label->setMargin(12);
        Label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        Label->setFocusPolicy(Qt::StrongFocus);
        information_ = Label; vbox_->addWidget(Label);
    }
    static_cast<QLabel*>(information_)->setText(Information);
    information_->show(); setFocusProxy(information_);
}
bool CQuickView::RequestClose() { return !surface_ || surface_->RequestClose(); }
bool CQuickView::IsPinned() const { return surface_ && surface_->IsPinned(); }
void CQuickView::CloseView() { cancelProbe(); if (surface_) surface_->Close(); if (information_) information_->hide(); }
void CQuickView::EnsureKeyboardFocusOnView() const
{ if (surface_ && surface_->isVisible()) surface_->Pin(); if (focusProxy()) focusProxy()->setFocus(); }

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



void CGridView::SetAutomaticColumns(bool Automatic)
{
    automaticColumnRule_ = -1;
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
    const auto Library = hc::columns::WithBuiltInProfiles(Columns["ColumnsV2/Profiles"].toObject());
    const auto Colors = Committed["options"].toObject()["fileColorRules"].toObject();
    const auto Path = QString::fromStdString(Model->WorkspaceLocation().root_);
    QHash<QString, QString> Resolved;
    QJsonObject ResolvedSignature;
    for (const auto& Value : Policy["rules"].toArray()) {
        const auto Rule = Value.toObject();
        if (!Rule["enabled"].toBool()) continue;
        const auto Token = hc::shell::ParseFolderToken(Rule["path"].toString());
        if (Token.kind_ == hc::shell::FolderToken::Kind::None || Token.kind_ == hc::shell::FolderToken::Kind::Invalid || Resolved.contains(Token.token_)) continue;
        Resolved[Token.token_] = hc::shell::ResolveFolderToken(Token).path_;
        ResolvedSignature[Token.token_] = Resolved[Token.token_];
    }
    const auto Signature = QByteArray::number(Model->DirectoryGeneration()) + Path.toUtf8() + QJsonDocument(Policy).toJson(QJsonDocument::Compact) + QJsonDocument(Library).toJson(QJsonDocument::Compact) + QJsonDocument(Colors).toJson(QJsonDocument::Compact) + QJsonDocument(ResolvedSignature).toJson(QJsonDocument::Compact);
    if (Signature == automaticColumnsSignature_) return;
    automaticColumnsSignature_ = Signature; automaticColumnsStop_.request_stop(); automaticColumnsStop_ = std::stop_source{};
    const auto Generation = ++automaticColumnsGeneration_;
    using AutoResult = std::pair<hc::storage::Result<std::optional<hc::columns::ColumnProfile>>, int>;
    auto* Watcher = new QFutureWatcher<AutoResult>(this);
    const auto DirectoryGeneration = Model->DirectoryGeneration();
    connect(Watcher, &QFutureWatcher<AutoResult>::finished, this, [this, Watcher, Generation, DirectoryGeneration] {
        const auto [Result, MatchedRule] = Watcher->result(); Watcher->deleteLater();
        auto* Current = qobject_cast<CFSModel*>(model_);
        if (Generation != automaticColumnsGeneration_ || manualColumns_ || !Current || Current->IsLoading() || Current->DirectoryGeneration() != DirectoryGeneration) return;
        if (IsEditing()) { automaticColumnsSignature_.clear(); requestPresentation(); return; }
        if (!Result.HasValue()) { setProperty("hc.columns.autoError", QString::fromStdString(Result.Failure().code_)); return; }
        setProperty("hc.columns.autoError", QVariant{});
        const bool SourceChanged = automaticColumnRule_ != MatchedRule;
        automaticColumnRule_ = MatchedRule;
        auto Next = Result.Value();
        if (Next && !automaticColumnsBase_) automaticColumnsBase_ = ColumnProfile();
        if (!Next && automaticColumnsBase_) Next = automaticColumnsBase_;
        auto Comparable = ColumnProfile();
        // Re-evaluating the same automatic set must not undo tab width overlays
        // retained by a settings synchronization. Widths are not set identity.
        if (Next && Next->id_ == Comparable.id_) for (auto& Definition : Comparable.columns_)
            for (const auto& Expected : Next->columns_) if (Definition.id_ == Expected.id_) for (int Mode = 0; Mode < 3; ++Mode) {
                const bool Hidden = Definition.widths_[Mode].hidden_; Definition.widths_[Mode] = Expected.widths_[Mode]; Definition.widths_[Mode].hidden_ = Hidden;
            }
        if (Next && hc::columns::EncodeProfile(*Next) != hc::columns::EncodeProfile(Comparable)) {
            const QScopedValueRollback Guard(applyingAutoColumns_, true);
            ApplyColumnProfile(*Next); emit sigViewSettingsChanged();
        }
        if (!Result.Value()) automaticColumnsBase_.reset();
        if (SourceChanged) emit sigViewSettingsChanged();
    });
    const auto Index = Model->DirectoryEntries();
    hc::storage::OperationContext Context; Context.stopToken_ = automaticColumnsStop_.get_token();
    Watcher->setFuture(QtConcurrent::run([Policy, Library, Colors, Path, Index, Context, Resolved] {
        int MatchedRule = -1;
        auto Result = hc::columns::SelectAutomaticColumns(Policy, Library, Colors, Path, Index, Context, Resolved, &MatchedRule);
        return AutoResult{std::move(Result), MatchedRule};
    }));
}
