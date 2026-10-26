#include "stdafx.h"
#include "dlgFavoriteDir.hpp"
#include "UniqueLibs/qtMessageDialog.hpp"

#include "cmnHCUtils.hpp"
#include "solFavoriteDirMgr.hpp"

#include <fmstyle/Glyphs.h>
#include <fmstyle/StylePaint.h>
#include <fmstyle/StyleProps.h>
#include <fmstyle/ThemeManager.h>
#include <fmwidgets/Card.h>
#include <fmwidgets/WindowChrome.h>
#include <QFileDialog>
#include <QStyledItemDelegate>

namespace {
// Two-line favorite row: number · folder icon · name, then the command target path in the fixed-width font.
// The item text stays the plain name (the slots look items up by it); the path comes from Qt::UserRole.
class FavoriteItemDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem& Option, const QModelIndex& Index) const override {
        return QSize(QStyledItemDelegate::sizeHint(Option, Index).width(), Option.fontMetrics.height() * 2 + 12);
    }
    void paint(QPainter* Painter, const QStyleOptionViewItem& Option, const QModelIndex& Index) const override {
        QStyleOptionViewItem Base = Option; initStyleOption(&Base, Index); Base.text.clear(); Base.icon = {};
        Option.widget->style()->drawControl(QStyle::CE_ItemViewItem, &Base, Painter, Option.widget);
        const auto& Colors = fm::style::themeColorsFor(Option.widget);
        using fm::style::Token;
        const bool Selected = Option.state & QStyle::State_Selected;
        const bool OnFill = Selected && Colors.fillsSelection() && (Option.state & QStyle::State_Active);
        const QRect R = Option.rect.adjusted(10, 4, -8, -4);
        const int Line = Option.fontMetrics.height();
        Painter->save();
        Painter->setPen(OnFill ? Colors[Token::OnAccent] : Colors[Token::Fg3]);
        Painter->drawText(QRect(R.left(), R.top(), 16, Line), Qt::AlignLeft | Qt::AlignVCenter, QString::number(Index.row() + 1));
        const auto Path = Index.data(Qt::UserRole).toString();
        const bool Network = Path.startsWith(QStringLiteral("\\\\")) || Path.startsWith(QStringLiteral("//"));
        fm::style::paintGlyph(Painter, Network ? fm::style::Glyph::Drive : fm::style::Glyph::Folder, QRectF(R.left() + 20, R.top() + (Line - 16) / 2.0, 16, 16),
            OnFill ? Colors[Token::OnAccent] : Network ? Colors[Token::KExe] : Colors[Token::Folder]);
        const int Left = R.left() + 44;
        Painter->setPen(OnFill ? Colors[Token::OnAccent] : Colors[Token::Fg]);
        Painter->drawText(QRect(Left, R.top(), R.right() - Left, Line), Qt::AlignLeft | Qt::AlignVCenter, Option.fontMetrics.elidedText(Index.data().toString(), Qt::ElideRight, R.right() - Left));
        auto Mono = fm::style::monoFont(11); Painter->setFont(Mono);
        Painter->setPen(OnFill ? Colors[Token::OnAccent] : Colors[Token::Fg3]);
        Painter->drawText(QRect(Left, R.top() + Line + 2, R.right() - Left, Line), Qt::AlignLeft | Qt::AlignVCenter,
            QFontMetrics(Mono).elidedText(QDir::toNativeSeparators(Path), Qt::ElideMiddle, R.right() - Left));
        Painter->restore();
    }
};
}

DECLARE_CMNLIBSV2_NAMESPACE

QFavoriteDirConfigure::QFavoriteDirConfigure( QWidget* Parent, Qt::WindowFlags Flags )
    : nsHC::QBaseUI( Parent, Flags )
{
    ui.setupUi( this );

    const auto Favorites = TyStFavoriteDirMgr::GetInstance();
    for( qsizetype Index = 0; Index < Favorites->GetCount(); ++Index )
        items_.append( Favorites->GetItem( Index ) );
    drivesOn_ = Favorites->GetDrivesOn();
    // Populate controls without invoking persistence through toggled signals.
    const QSignalBlocker None( ui.rdoDrivesOnNone ), Top( ui.rdoDrivesOnTop ), Bottom( ui.rdoDrivesOnBottom );
    ui.rdoDrivesOnNone->setChecked( drivesOn_ == CFavoriteDirMgr::DRIVES_ON_NONE );
    ui.rdoDrivesOnTop->setChecked( drivesOn_ == CFavoriteDirMgr::DRIVES_ON_TOP );
    ui.rdoDrivesOnBottom->setChecked( drivesOn_ == CFavoriteDirMgr::DRIVES_ON_BOTTOM );
    connect( ui.edtCommand, &QLineEdit::textEdited, this, [this] { on_edtCommand_editingFinished(); } );
    connect( ui.edtPath, &QLineEdit::textEdited, this, [this] { on_edtPath_editingFinished(); ui.lstDir->viewport()->update(); } );
    arrangeLayout();
    refresh();
}

void QFavoriteDirConfigure::arrangeLayout()
{
    // 32 px title bar + close, like every other dialog (the old frame had its own literal X).
    setWindowTitle( tr( "폴더 즐겨찾기" ) );
    ui.frmTitleBar->hide();
    auto* Outer = qobject_cast<QVBoxLayout*>( layout() );
    Outer->insertWidget( 0, new fm::ui::WindowChrome( this, this, { false, false } ) );
    auto* Body = new QWidget( this ); auto* Columns = new QHBoxLayout( Body ); Columns->setContentsMargins( 16, 16, 16, 16 ); Columns->setSpacing( 12 );
    Outer->insertWidget( 1, Body, 1 );

    // Left: items card with compact actions in its header band.
    auto* Items = new fm::ui::Card( tr( "항목" ), Body ); Items->setMeta( tr( "↑↓ 순서 변경 · 1~9 번호 키" ) ); Items->setMinimumWidth( 300 );
    auto* ItemRows = new QVBoxLayout( Items ); ItemRows->setContentsMargins( 0, 0, 0, 0 );
    ui.lstDir->setFrameShape( QFrame::NoFrame ); ui.lstDir->setItemDelegate( new FavoriteItemDelegate( ui.lstDir ) ); ItemRows->addWidget( ui.lstDir );
    auto* Actions = new QWidget( Items ); auto* ActionRow = new QHBoxLayout( Actions ); ActionRow->setContentsMargins( 0, 0, 0, 0 ); ActionRow->setSpacing( 4 );
    ui.btnAdd->setText( tr( "추가(&A)" ) ); ui.btnRemove->setText( tr( "삭제(&D)" ) );
    moveUp_ = new QPushButton( QStringLiteral( "↑" ), Actions ); moveUp_->setObjectName( "hc.favorites.moveUp" ); moveUp_->setToolTip( tr( "위로 이동" ) ); moveUp_->setAccessibleName( tr( "위로 이동" ) );
    moveDown_ = new QPushButton( QStringLiteral( "↓" ), Actions ); moveDown_->setObjectName( "hc.favorites.moveDown" ); moveDown_->setToolTip( tr( "아래로 이동" ) ); moveDown_->setAccessibleName( tr( "아래로 이동" ) );
    for( auto* Button : { ui.btnAdd, ui.btnRemove, moveUp_, moveDown_ } ) { Button->setParent( Actions ); fm::style::setSmall( Button ); Button->setMinimumWidth( 0 ); Button->setAutoDefault( false ); ActionRow->addWidget( Button ); }
    Items->setHeaderWidget( Actions );
    connect( moveUp_, &QPushButton::clicked, this, [this] { moveCurrent( -1 ); } );
    connect( moveDown_, &QPushButton::clicked, this, [this] { moveCurrent( 1 ); } );
    Columns->addWidget( Items, 1 );

    // Right: the selected item and the drive list position.
    auto* Side = new QVBoxLayout; Side->setSpacing( 12 ); Columns->addLayout( Side, 1 );
    auto* Selected = new fm::ui::Card( tr( "선택한 항목" ), Body ); Side->addWidget( Selected );
    auto* Grid = new QGridLayout( Selected ); Grid->setContentsMargins( 14, 12, 14, 12 ); Grid->setHorizontalSpacing( 8 ); Grid->setVerticalSpacing( 8 );
    nameView_ = new QLineEdit( Selected ); nameView_->setObjectName( "hc.favorites.name" ); nameView_->setReadOnly( true ); nameView_->setAccessibleName( tr( "이름" ) );
    auto* NameLabel = new QLabel( tr( "이름" ), Selected ); NameLabel->setBuddy( nameView_ );
    Grid->addWidget( NameLabel, 0, 0 ); Grid->addWidget( nameView_, 0, 1 ); Grid->addWidget( ui.btnRename, 0, 2 );
    auto* CommandLabel = new QLabel( tr( "명령어(&C)" ), Selected ); CommandLabel->setBuddy( ui.edtCommand );
    ui.btnSelectCMD->setText( tr( "선택…" ) ); ui.btnSelectCMD->setMinimumSize( 0, 0 ); ui.btnSelectCMD->setMaximumSize( QWIDGETSIZE_MAX, QWIDGETSIZE_MAX );
    ui.btnSelectCMD->setToolTip( tr( "폴더를 골라 'cd 경로' 명령을 넣습니다." ) );
    Grid->addWidget( CommandLabel, 1, 0 ); Grid->addWidget( ui.edtCommand, 1, 1 ); Grid->addWidget( ui.btnSelectCMD, 1, 2 );
    auto* PathLabel = new QLabel( tr( "대상 경로" ), Selected ); PathLabel->setBuddy( ui.edtPath );
    browse_ = new QPushButton( tr( "찾아보기…" ), Selected ); browse_->setObjectName( "hc.favorites.browse" ); browse_->setAutoDefault( false );
    Grid->addWidget( PathLabel, 2, 0 ); Grid->addWidget( ui.edtPath, 2, 1 ); Grid->addWidget( browse_, 2, 2 );
    auto MonoFont = fm::style::monoFont( 12 ); ui.edtCommand->setFont( MonoFont ); ui.edtPath->setFont( MonoFont );
    auto* Hint = new QLabel( tr( "명령어는 'cd 경로'처럼 씁니다. 경로는 환경 변수와 UNC를 그대로 씁니다." ), Selected ); Hint->setWordWrap( true );
    Grid->addWidget( Hint, 3, 0, 1, 3 ); Grid->setColumnStretch( 1, 1 );
    connect( ui.btnSelectCMD, &QPushButton::clicked, this, [this] {
        const auto Folder = QFileDialog::getExistingDirectory( this, tr( "이동할 폴더" ), ui.edtPath->text() );
        if( Folder.isEmpty() ) return;
        ui.edtCommand->setText( QStringLiteral( "cd \"%1\"" ).arg( QDir::toNativeSeparators( Folder ) ) ); on_edtCommand_editingFinished();
    } );
    connect( browse_, &QPushButton::clicked, this, [this] {
        const auto Folder = QFileDialog::getExistingDirectory( this, tr( "대상 경로" ), ui.edtPath->text() );
        if( Folder.isEmpty() ) return;
        ui.edtPath->setText( QDir::toNativeSeparators( Folder ) ); on_edtPath_editingFinished(); ui.lstDir->viewport()->update();
    } );

    auto* Drives = new fm::ui::Card( tr( "드라이브 목록" ), Body ); Side->addWidget( Drives );
    auto* DriveRows = new QVBoxLayout( Drives ); DriveRows->setContentsMargins( 14, 12, 14, 12 ); DriveRows->setSpacing( 8 );
    auto* DriveLine = new QHBoxLayout; DriveLine->setSpacing( 12 ); DriveLine->addWidget( new QLabel( tr( "팝업 위치" ), Drives ) );
    for( auto* Choice : { ui.rdoDrivesOnNone, ui.rdoDrivesOnTop, ui.rdoDrivesOnBottom } ) { Choice->setParent( Drives ); DriveLine->addWidget( Choice ); }
    DriveLine->addStretch(); DriveRows->addLayout( DriveLine );
    auto* DriveHint = new QLabel( tr( "드라이브는 하위 메뉴로 접히며, 항목 번호 키 1~9는 즐겨찾기 순서를 따릅니다." ), Drives ); DriveHint->setWordWrap( true ); DriveRows->addWidget( DriveHint );
    Side->addStretch();

    // Footer: count status at the left, OK (primary) · Cancel at the right.
    auto* Footer = new QFrame( this ); Footer->setFrameShape( QFrame::StyledPanel ); fm::style::setFooter( Footer );
    auto* FooterRow = new QHBoxLayout( Footer ); FooterRow->setContentsMargins( 16, 10, 16, 10 ); FooterRow->setSpacing( 8 );
    status_ = new QLabel( Footer ); status_->setObjectName( "hc.favorites.status" ); FooterRow->addWidget( status_, 1 );
    for( auto* Button : { ui.btnOK, ui.btnCancel } ) { Button->setParent( Footer ); FooterRow->addWidget( Button ); }
    fm::style::setButtonRole( ui.btnOK, fm::style::ButtonRole::Primary ); ui.btnOK->setDefault( true );
    Outer->insertWidget( 2, Footer );
    ui.frmContent->hide(); ui.frmFooter->hide();
    resize( 720, 480 );
}

void QFavoriteDirConfigure::moveCurrent( int Offset )
{
    const int Row = ui.lstDir->currentRow(), Target = Row + Offset;
    if( Row < 0 || Target < 0 || Target >= items_.size() ) return;
    on_edtCommand_editingFinished();
    items_.swapItemsAt( Row, Target );
    refresh();
    ui.lstDir->setCurrentRow( Target );
}

void QFavoriteDirConfigure::on_btnClose_clicked( bool checked )
{
    on_btnCancel_clicked( checked );
}

void QFavoriteDirConfigure::on_lstDir_currentItemChanged( QListWidgetItem* Current, QListWidgetItem* Previous )
{
    UNREFERENCED_PARAMETER( Current );
    UNREFERENCED_PARAMETER( Previous );

    const auto Name = Current ? Current->text() : QString{};
    refreshItem( Name );
}

QString QFavoriteDirConfigure::promptName( const QString& Initial, int ExcludedRow )
{
    QString Candidate = Initial;
    for( ;; )
    {
        bool Accepted = false;
        Candidate = hc::ui::InputDialog::getText( this, tr( "즐겨찾기 이름" ), tr( "항목의 이름을 지정해 주세요:" ),
            QLineEdit::Normal, Candidate, &Accepted ).trimmed();
        if( !Accepted ) return {};
        bool Valid = !Candidate.isEmpty() && !Candidate.contains( QRegularExpression( "[\\r\\n\\x{2028}\\x{2029}]" ) );
        for( qsizetype Index = 0; Index < items_.size(); ++Index )
            if( Index != ExcludedRow && items_[ Index ].Name.compare( Candidate, Qt::CaseInsensitive ) == 0 ) Valid = false;
        if( Valid ) return Candidate;
        ::ShowMSGBox( this, QMessageBox::Information, tr( "빈 이름, 여러 줄 이름 또는 이미 있는 이름은 사용할 수 없습니다." ) );
    }
}

void QFavoriteDirConfigure::on_btnAdd_clicked( bool checked )
{
    const auto Name = promptName( tr( "새 항목" ) );
    if( Name.isEmpty() ) return;
    items_.append( { Name, "", "" } );
    ui.lstDir->addItem( Name );
    ui.lstDir->setCurrentRow( ui.lstDir->count() - 1 );
    if( status_ ) status_->setText( tr( "항목 %1개 · 확인으로 저장 · 취소로 이전 내용 유지" ).arg( items_.size() ) );
}

void QFavoriteDirConfigure::on_btnRemove_clicked( bool checked )
{
    const auto Current = ui.lstDir->currentItem();
    if( Current == nullptr )
        return;

    items_.removeAt( ui.lstDir->row( Current ) );

    refresh();
    refreshItem( "" );
}

void QFavoriteDirConfigure::on_btnRename_clicked( bool checked )
{
    const auto Current = ui.lstDir->currentItem();
    if( Current == nullptr ) return;
    on_edtCommand_editingFinished();
    const auto Name = promptName( Current->text(), ui.lstDir->currentRow() );
    if( Name.isEmpty() ) return;
    items_[ ui.lstDir->currentRow() ].Name = Name;
    Current->setText( Name );
    if( nameView_ ) nameView_->setText( Name );
}

void QFavoriteDirConfigure::on_edtCommand_editingFinished()
{
    CFavoriteDirMgr::TyFavoriteDir Item;

    const auto Current = ui.lstDir->currentItem();
    if( Current == nullptr ) return;

    Item.Name       = Current->text();
    Item.Command    = ui.edtCommand->text();
    Item.Path       = ui.edtPath->text();

    items_[ ui.lstDir->currentRow() ] = Item;
    Current->setData( Qt::UserRole, Item.Path );
}

void QFavoriteDirConfigure::on_edtPath_editingFinished()
{
    on_edtCommand_editingFinished();
}

void QFavoriteDirConfigure::on_btnOK_clicked( bool checked )
{
    on_edtCommand_editingFinished();

    if( !TyStFavoriteDirMgr::GetInstance()->ApplyConfiguration( items_, drivesOn_ ) )
    {
        ::ShowMSGBox( this, QMessageBox::Warning, tr( "즐겨찾기를 저장하지 못했습니다. 설정 파일의 쓰기 권한을 확인한 뒤 다시 시도하세요." ) );
        return;
    }
    done( Accepted );
}

void QFavoriteDirConfigure::on_btnCancel_clicked( bool checked )
{
    done( Rejected );
}

void QFavoriteDirConfigure::on_rdoDrivesOnNone_toggled( bool checked )
{

    if( checked ) drivesOn_ = CFavoriteDirMgr::DRIVES_ON_NONE;
}

void QFavoriteDirConfigure::on_rdoDrivesOnTop_toggled( bool checked )
{

    if( checked ) drivesOn_ = CFavoriteDirMgr::DRIVES_ON_TOP;
}

void QFavoriteDirConfigure::on_rdoDrivesOnBottom_toggled( bool checked )
{

    if( checked ) drivesOn_ = CFavoriteDirMgr::DRIVES_ON_BOTTOM;
}

void QFavoriteDirConfigure::refresh()
{

    QSignalBlocker Blocker( ui.lstDir );
    ui.lstDir->clear();

    for( const auto& Item : items_ )
    {
        auto* Row = new QListWidgetItem( Item.Name, ui.lstDir );
        Row->setData( Qt::UserRole, Item.Path ); Row->setToolTip( QDir::toNativeSeparators( Item.Path ) );
    }
    if( status_ ) status_->setText( tr( "항목 %1개 · 확인으로 저장 · 취소로 이전 내용 유지" ).arg( items_.size() ) );

    ui.lstDir->setCurrentItem( nullptr );
    refreshItem( {} );
}

void QFavoriteDirConfigure::refreshItem( const QString& Name )
{
    const bool HasItem = !Name.isEmpty();
    ui.edtCommand->setEnabled( HasItem );
    ui.edtPath->setEnabled( HasItem );
    ui.btnSelectCMD->setEnabled( HasItem );
    ui.btnRemove->setEnabled( HasItem );
    ui.btnRename->setEnabled( HasItem );
    if( browse_ ) browse_->setEnabled( HasItem );
    const int Row = ui.lstDir->currentRow();
    if( moveUp_ ) moveUp_->setEnabled( HasItem && Row > 0 );
    if( moveDown_ ) moveDown_->setEnabled( HasItem && Row >= 0 && Row + 1 < items_.size() );
    if( nameView_ ) nameView_->setText( Name );

    ui.edtCommand->setText( "" );
    ui.edtPath->setText( "" );

    if( Name.isEmpty() == true )
        return;

    const auto Item = items_.value( ui.lstDir->currentRow() );

    ui.edtCommand->setText( Item.Command );
    ui.edtPath->setText( Item.Path );
}
