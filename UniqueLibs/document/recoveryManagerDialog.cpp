#include "stdafx.h"
#include "recoveryManagerDialog.hpp"

/// 모듈
#include "UniqueLibs/qtDialogFrame.hpp"
#include "UniqueLibs/qtMessageDialog.hpp"

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리
#include <fmwidgets/FindBox.h>
#include <fmwidgets/Glyph.h>
#include <fmwidgets/SegmentedControl.h>
#include <fmwidgets/Tag.h>

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QtConcurrentRun>

DECLARE_CMNLIBSV2_NAMESPACE

namespace hc::document
{
    namespace
    {
        enum Column { CheckColumn, NameColumn, KindColumn, SavedColumn, ExpiresColumn, SizeColumn, StateColumn, ColumnCount };
        QString Size( qint64 Bytes )
        {
            return Bytes >= 1024 * 1024 ? QStringLiteral( "%1 MB" ).arg( Bytes / 1048576.0, 0, 'f', 1 )
                                        : QStringLiteral( "%1 KB" ).arg( qMax< qint64 >( 1, ( Bytes + 1023 ) / 1024 ) );
        }
        QString Capacity( qint64 Bytes )
        {
            if( Bytes <= 0 ) return {};
            return Bytes >= 1024LL * 1024 * 1024 && Bytes % ( 1024LL * 1024 * 1024 ) == 0
                ? QStringLiteral( "%1 GiB" ).arg( Bytes / ( 1024LL * 1024 * 1024 ) ) : QStringLiteral( "%1 MiB" ).arg( Bytes / ( 1024 * 1024 ) );
        }
        QString Stamp( const QDateTime& Time, const QString& Format ) { return Time.isValid() ? QLocale().toString( Time.toLocalTime(), Format ) : QStringLiteral( "—" ); }
        bool Documents( const recovery::Item& Entry ) { return Entry.kind_ == recovery::Kind::Edit || Entry.kind_ == recovery::Kind::Untitled; }
    }

    QString RecoveryManagerDialog::ArchiveMessage( const QString& Code )
    {
        if( Code == QStringLiteral( "archive-entry-changed" ) ) return tr( "편집을 시작한 뒤 압축 파일 안의 원본이 바뀌어 반영하지 않았습니다. 별도 문서로 열어 비교하십시오." );
        if( Code == QStringLiteral( "archive-missing" ) ) return tr( "압축 파일을 찾을 수 없습니다." );
        if( Code == QStringLiteral( "archive-entry-missing" ) ) return tr( "압축 파일 안에서 원래 파일을 찾을 수 없습니다." );
        if( Code == QStringLiteral( "archive-size" ) ) return tr( "64 MiB보다 큰 압축 파일은 이 창에서 반영할 수 없습니다." );
        if( Code == QStringLiteral( "archive-format" ) ) return tr( "ZIP 이외 형식은 아직 이 창에서 반영할 수 없습니다." );
        if( Code == QStringLiteral( "archive-nested" ) ) return tr( "압축 파일 안의 압축 파일은 이 창에서 반영할 수 없습니다." );
        if( Code == QStringLiteral( "archive-source-unknown" ) || Code == QStringLiteral( "archive-draft-manifest" ) ) return tr( "어느 압축 파일에서 나온 편집본인지 알 수 없습니다." );
        if( Code == QStringLiteral( "archive-draft-in-use" ) ) return tr( "편집본을 쓰고 있는 창이 아직 열려 있습니다." );
        if( Code == QStringLiteral( "archive-write" ) || Code == QStringLiteral( "archive-read" ) ) return tr( "압축 파일을 읽거나 쓰지 못했습니다. 다른 프로그램이 쓰고 있는지 확인하십시오." );
        if( Code.startsWith( QStringLiteral( "archive-zip-" ) ) ) return tr( "이 ZIP 파일은 이 창에서 반영할 수 없습니다(%1)." ).arg( Code.mid( 12 ) );
        return tr( "압축 파일에 반영하지 못했습니다: %1" ).arg( Code );
    }

    RecoveryManagerDialog::RecoveryManagerDialog( QString Root, recovery::Policy Settings, Hooks Calls, QWidget* Parent )
        : QDialog( Parent ), root_( std::move( Root ) ), policy_( std::move( Settings ) ), hooks_( std::move( Calls ) )
    {
        setObjectName( QStringLiteral( "recoveryManager" ) );
        setWindowTitle( tr( "복구 사본 관리" ) );
        auto* Layout = new QVBoxLayout( this ); Layout->setSpacing( 8 );
        summary_ = new QLabel( this ); summary_->setObjectName( QStringLiteral( "recoveryManager.summary" ) ); summary_->setForegroundRole( QPalette::PlaceholderText );
        summary_->setTextFormat( Qt::RichText ); summary_->setWordWrap( true );
        connect( summary_, &QLabel::linkActivated, this, [ this ] { emit SettingsRequested(); } );
        Layout->addWidget( summary_ );

        auto* Tools = new QHBoxLayout; Tools->setSpacing( 10 );
        filter_ = new fm::ui::SegmentedControl( this ); filter_->setObjectName( QStringLiteral( "recoveryManager.filter" ) );
        filter_->setItems( { tr( "전체" ), tr( "문서" ), tr( "EPUB 초안" ), tr( "압축 편집본" ) } );
        find_ = new fm::ui::FindBox( this ); find_->setObjectName( QStringLiteral( "recoveryManager.find" ) );
        find_->setPlaceholderText( tr( "이름이나 경로로 찾기" ) ); find_->setClearButtonEnabled( true ); find_->setMinimumWidth( 220 );
        Tools->addWidget( filter_ ); Tools->addStretch( 1 ); Tools->addWidget( find_ );
        Layout->addLayout( Tools );

        table_ = new QTableWidget( 0, ColumnCount, this ); table_->setObjectName( QStringLiteral( "recoveryCopies" ) );
        table_->setHorizontalHeaderLabels( { QString{}, tr( "문서" ), tr( "종류" ), tr( "보존" ), tr( "만료" ), tr( "크기" ), tr( "상태" ) } );
        table_->setSelectionBehavior( QAbstractItemView::SelectRows ); table_->setSelectionMode( QAbstractItemView::SingleSelection );
        table_->setEditTriggers( QAbstractItemView::NoEditTriggers ); table_->verticalHeader()->hide(); table_->setShowGrid( false );
        table_->horizontalHeader()->setSectionResizeMode( QHeaderView::ResizeToContents );
        table_->horizontalHeader()->setSectionResizeMode( NameColumn, QHeaderView::Stretch );
        table_->verticalHeader()->setDefaultSectionSize( 44 );
        Layout->addWidget( table_, 1 );
        empty_ = new QLabel( this ); empty_->setAlignment( Qt::AlignCenter ); empty_->setForegroundRole( QPalette::PlaceholderText ); empty_->hide();
        Layout->addWidget( empty_ );
        detail_ = new QLabel( this ); detail_->setObjectName( QStringLiteral( "recoveryManager.detail" ) ); detail_->setWordWrap( true );
        detail_->setTextFormat( Qt::PlainText ); detail_->setMinimumHeight( detail_->fontMetrics().height() * 2 + 12 );
        detail_->setFrameShape( QFrame::StyledPanel ); detail_->setMargin( 6 );
        Layout->addWidget( detail_ );

        auto* Footer = hc::ui::ApplyDialogFrame( this, { tr( "복구 사본 관리" ), tr( "원본을 덮어쓰지 않고 별도 문서로 엽니다." ), fm::ui::glyph::Clock } );
        if( auto* Stretch = Footer->takeAt( 0 ) ) delete Stretch;
        buttons_ = new QWidget( Footer->parentWidget() ); auto* Normal = new QHBoxLayout( buttons_ ); Normal->setContentsMargins( 0, 0, 0, 0 ); Normal->setSpacing( 8 );
        delete_ = new QPushButton( tr( "삭제…" ), buttons_ ); delete_->setObjectName( QStringLiteral( "recoveryManager.delete" ) );
        count_ = new QLabel( buttons_ ); count_->setForegroundRole( QPalette::PlaceholderText ); count_->setObjectName( QStringLiteral( "recoveryManager.count" ) );
        apply_ = new QPushButton( tr( "압축 파일에 반영…" ), buttons_ ); apply_->setObjectName( QStringLiteral( "recoveryManager.apply" ) );
        open_ = new QPushButton( tr( "별도 문서로 열기" ), buttons_ ); open_->setObjectName( QStringLiteral( "recoveryManager.open" ) );
        fm::style::setButtonRole( open_, fm::style::ButtonRole::Primary );
        auto* Close = new QPushButton( tr( "닫기" ), buttons_ ); Close->setObjectName( QStringLiteral( "recoveryManager.close" ) );
        Normal->addWidget( delete_ ); Normal->addWidget( count_ ); Normal->addStretch( 1 ); Normal->addWidget( apply_ ); Normal->addWidget( open_ ); Normal->addWidget( Close );
        confirm_ = new QWidget( Footer->parentWidget() ); auto* Confirm = new QHBoxLayout( confirm_ ); Confirm->setContentsMargins( 0, 0, 0, 0 ); Confirm->setSpacing( 8 );
        confirmText_ = new QLabel( confirm_ ); confirmText_->setWordWrap( true ); confirmText_->setObjectName( QStringLiteral( "recoveryManager.confirmText" ) );
        auto* Back = new QPushButton( tr( "돌아가기" ), confirm_ ); Back->setObjectName( QStringLiteral( "recoveryManager.back" ) );
        auto* Erase = new QPushButton( tr( "삭제" ), confirm_ ); Erase->setObjectName( QStringLiteral( "recoveryManager.confirmDelete" ) );
        fm::style::setButtonRole( Erase, fm::style::ButtonRole::Danger );
        Confirm->addWidget( confirmText_, 1 ); Confirm->addWidget( Back ); Confirm->addWidget( Erase ); confirm_->hide();
        Footer->addWidget( buttons_, 1 ); Footer->addWidget( confirm_, 1 );

        connect( Close, &QPushButton::clicked, this, &QDialog::reject );
        connect( filter_, &fm::ui::SegmentedControl::currentIndexChanged, this, [ this ] { applyFilter(); } );
        connect( find_, &QLineEdit::textChanged, this, [ this ] { applyFilter(); } );
        connect( table_, &QTableWidget::cellClicked, this, [ this ]( int Row, int Column ) {
            const int Index = table_->item( Row, CheckColumn ) ? table_->item( Row, CheckColumn )->data( Qt::UserRole ).toInt() : -1;
            if( Index < 0 ) return;
            if( Column != CheckColumn && ( table_->item( Row, CheckColumn )->flags() & Qt::ItemIsUserCheckable ) )
                table_->item( Row, CheckColumn )->setCheckState( table_->item( Row, CheckColumn )->checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked );
            showDetail( Index );
        } );
        connect( table_, &QTableWidget::itemChanged, this, [ this ]( QTableWidgetItem* Item ) { if( Item->column() == CheckColumn ) refreshButtons(); } );
        connect( delete_, &QPushButton::clicked, this, [ this, Back ] { buttons_->hide(); confirm_->show(); Back->setFocus(); } );
        connect( Back, &QPushButton::clicked, this, [ this ] { confirm_->hide(); buttons_->show(); delete_->setFocus(); } );
        connect( Erase, &QPushButton::clicked, this, [ this ] {
            confirm_->hide(); buttons_->show();
            std::vector< recovery::Item > Targets; for( const int Index : checked() ) Targets.push_back( items_[ Index ] );
            if( Targets.empty() ) return;
            busy_ = true; refreshButtons();
            auto* Watcher = new QFutureWatcher< QStringList >( this );
            connect( Watcher, &QFutureWatcherBase::finished, this, [ this, Watcher, Count = int( Targets.size() ) ] {
                const auto Errors = Watcher->result(); Watcher->deleteLater(); busy_ = false;
                Reload();
                report( Errors.isEmpty() ? tr( "선택한 사본 %1개를 삭제했습니다." ).arg( Count )
                                         : tr( "사본 %1개를 삭제하지 못했습니다. 다른 창에서 쓰고 있거나 저장소를 바꿀 수 없습니다." ).arg( Errors.size() ) );
            } );
            Watcher->setFuture( QtConcurrent::run( [ Root = root_, Targets ] {
                QStringList Errors;
                for( const auto& Entry : Targets ) if( const auto Error = recovery::RemoveCopy( Root, Entry ); !Error.isEmpty() ) Errors.append( Error );
                return Errors;
            } ) );
        } );
        connect( open_, &QPushButton::clicked, this, [ this ] {
            QStringList Errors; int Opened = 0;
            for( const int Index : checked() ) {
                const auto Error = hooks_.open_ ? hooks_.open_( items_[ Index ] ) : tr( "열 수 없습니다." );
                if( Error.isEmpty() ) ++Opened; else Errors.append( items_[ Index ].name_ + QStringLiteral( ": " ) + Error );
            }
            if( Errors.isEmpty() && Opened ) { accept(); return; }
            report( Errors.join( QChar( '\n' ) ) );
        } );
        connect( apply_, &QPushButton::clicked, this, [ this ] {
            const auto Selected = checked(); if( Selected.size() != 1 ) return;
            const auto Entry = items_[ Selected.front() ];
            if( hc::ui::MessageDialog::question( this, tr( "압축 파일에 반영" ),
                    tr( "%1의 %2을(를) 이 편집본으로 바꿉니다. 계속하시겠습니까?" ).arg( Entry.location_, Entry.name_ ), QMessageBox::Yes | QMessageBox::Cancel ) != QMessageBox::Yes ) return;
            busy_ = true; refreshButtons(); report( tr( "압축 파일에 반영하고 있습니다…" ) );
            auto* Watcher = new QFutureWatcher< QString >( this );
            connect( Watcher, &QFutureWatcherBase::finished, this, [ this, Watcher, Name = Entry.name_ ] {
                const auto Error = Watcher->result(); Watcher->deleteLater(); busy_ = false;
                Reload();
                report( Error.isEmpty() ? tr( "%1을(를) 압축 파일에 반영하고 편집본을 정리했습니다." ).arg( Name ) : ArchiveMessage( Error ) );
            } );
            Watcher->setFuture( QtConcurrent::run( [ Directory = Entry.key_ ] { return recovery::ApplyArchiveDraft( Directory ); } ) );
        } );
        resize( 900, 560 );
        Reload();
    }

    void RecoveryManagerDialog::SetFilter( const QString& Key )
    {
        const QStringList Keys{ QStringLiteral( "all" ), QStringLiteral( "documents" ), QStringLiteral( "epub" ), QStringLiteral( "archive" ) };
        filter_->setCurrentIndex( qMax( 0, int( Keys.indexOf( Key ) ) ) ); applyFilter();
    }

    void RecoveryManagerDialog::Reload()
    {
        busy_ = true; table_->setEnabled( false ); refreshButtons();
        summary_->setText( tr( "복구 사본을 읽는 중입니다…" ) );
        auto* Watcher = new QFutureWatcher< std::vector< recovery::Item > >( this );
        connect( Watcher, &QFutureWatcherBase::finished, this, [ this, Watcher ] {
            items_ = Watcher->result(); Watcher->deleteLater(); busy_ = false; table_->setEnabled( true );
            if( hooks_.inUse_ ) for( auto& Entry : items_ ) if( Entry.state_ != recovery::State::InUse && hooks_.inUse_( Entry ) ) { Entry.state_ = recovery::State::InUse; Entry.expires_ = {}; Entry.expired_ = false; }
            populate();
        } );
        Watcher->setFuture( QtConcurrent::run( [ Root = root_, Settings = policy_ ] { return recovery::ListItems( Root, Settings, QDateTime::currentDateTime() ); } ) );
    }

    void RecoveryManagerDialog::populate()
    {
        const QSignalBlocker Block( table_ );
        table_->clearContents(); table_->setRowCount( int( items_.size() ) );
        qint64 Total = 0; int DocumentCount = 0, EpubCount = 0, ArchiveCount = 0;
        for( int Row = 0; Row < int( items_.size() ); ++Row ) {
            const auto& Entry = items_[ Row ]; Total += Entry.bytes_;
            DocumentCount += Documents( Entry ); EpubCount += Entry.kind_ == recovery::Kind::Epub; ArchiveCount += Entry.kind_ == recovery::Kind::Archive;
            auto* Check = new QTableWidgetItem; Check->setData( Qt::UserRole, Row );
            if( Entry.state_ == recovery::State::InUse ) Check->setFlags( Qt::ItemIsEnabled | Qt::ItemIsSelectable );
            else { Check->setFlags( Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable ); Check->setCheckState( Qt::Unchecked ); }
            table_->setItem( Row, CheckColumn, Check );
            auto* Name = new QLabel( table_ ); Name->setTextFormat( Qt::RichText ); Name->setContentsMargins( 4, 2, 4, 2 );
            Name->setText( QStringLiteral( "<b>%1</b><br><span style=\"color:%3\">%2</span>" ).arg( Entry.name_.toHtmlEscaped(), Entry.location_.toHtmlEscaped(),
                Name->palette().color( QPalette::PlaceholderText ).name() ) );
            Name->setToolTip( Entry.location_ ); Name->setAttribute( Qt::WA_TransparentForMouseEvents );
            table_->setItem( Row, NameColumn, new QTableWidgetItem( Entry.name_ + QChar( '\n' ) + Entry.location_ ) );
            table_->setCellWidget( Row, NameColumn, Name );
            const QString Kind = Entry.kind_ == recovery::Kind::Edit ? tr( "편집 사본" ) : Entry.kind_ == recovery::Kind::Untitled ? tr( "새 문서" )
                : Entry.kind_ == recovery::Kind::Epub ? tr( "EPUB 초안" ) : tr( "압축 편집본" );
            table_->setItem( Row, KindColumn, new QTableWidgetItem( Kind ) );
            table_->setItem( Row, SavedColumn, new QTableWidgetItem( Stamp( Entry.saved_, QStringLiteral( "MM-dd HH:mm" ) ) ) );
            QString Expires = Entry.state_ == recovery::State::HotExit ? tr( "이어 갈 세션" ) : Entry.state_ == recovery::State::InUse ? QStringLiteral( "—" )
                : !Entry.expires_.isValid() ? tr( "무제한" ) : Entry.expired_ ? tr( "기간 지남" ) : Stamp( Entry.expires_, QStringLiteral( "MM-dd" ) );
            auto* ExpiresItem = new QTableWidgetItem( Expires );
            if( Entry.expired_ ) ExpiresItem->setForeground( fm::style::themeColorsFor( table_ )[ fm::style::Token::Danger ] );
            table_->setItem( Row, ExpiresColumn, ExpiresItem );
            auto* SizeItem = new QTableWidgetItem( Size( Entry.bytes_ ) ); SizeItem->setTextAlignment( Qt::AlignRight | Qt::AlignVCenter );
            table_->setItem( Row, SizeColumn, SizeItem );
            auto* Tag = new fm::ui::Tag( table_ ); Tag->setCompact( true );
            const QString State = Entry.state_ == recovery::State::Kept ? tr( "보관" ) : Entry.state_ == recovery::State::HotExit ? tr( "HotExit 대기" )
                : Entry.state_ == recovery::State::InUse ? tr( "사용 중" ) : tr( "압축에 반영 안 됨" );
            Tag->setText( State );
            Tag->setTone( Entry.state_ == recovery::State::HotExit ? fm::ui::Tag::Info : Entry.state_ == recovery::State::Pending ? fm::ui::Tag::Danger : fm::ui::Tag::Mute );
            auto* TagCell = new QWidget( table_ ); auto* TagLine = new QHBoxLayout( TagCell ); TagLine->setContentsMargins( 6, 0, 6, 0 ); TagLine->addWidget( Tag ); TagLine->addStretch( 1 );
            TagCell->setAttribute( Qt::WA_TransparentForMouseEvents );
            table_->setItem( Row, StateColumn, new QTableWidgetItem( State ) );
            table_->setCellWidget( Row, StateColumn, TagCell );
        }
        {
            const QSignalBlocker Keep( filter_ ); const int Current = filter_->currentIndex();
            filter_->setItems( { tr( "전체 %1" ).arg( items_.size() ), tr( "문서 %1" ).arg( DocumentCount ), tr( "EPUB 초안 %1" ).arg( EpubCount ), tr( "압축 편집본 %1" ).arg( ArchiveCount ) } );
            filter_->setCurrentIndex( qMax( 0, Current ) );
        }
        QStringList Policy;
        Policy << ( policy_.retentionDays_ > 0 ? tr( "보존 기간 %1일" ).arg( policy_.retentionDays_ ) : tr( "보존 기간 무제한" ) );
        Policy << ( policy_.maxBytes_ > 0 ? tr( "최대 %1 중 %2 사용" ).arg( Capacity( policy_.maxBytes_ ), Size( Total ) ) : tr( "%1 사용" ).arg( Size( Total ) ) );
        summary_->setText( Policy.join( QStringLiteral( " · " ) ) + QStringLiteral( " · <a href=\"settings\">%1</a>" ).arg( tr( "설정 변경" ) ) );
        current_ = -1; detail_->clear();
        applyFilter();
    }

    void RecoveryManagerDialog::applyFilter()
    {
        const int Filter = qMax( 0, filter_->currentIndex() );
        const auto Text = find_->text().trimmed();
        int Shown = 0; const QSignalBlocker Block( table_ );
        for( int Row = 0; Row < table_->rowCount(); ++Row ) {
            const auto* Check = table_->item( Row, CheckColumn ); if( !Check ) continue;
            const auto& Entry = items_[ Check->data( Qt::UserRole ).toInt() ];
            const bool Kind = Filter == 0 || ( Filter == 1 && Documents( Entry ) ) || ( Filter == 2 && Entry.kind_ == recovery::Kind::Epub ) || ( Filter == 3 && Entry.kind_ == recovery::Kind::Archive );
            const bool Match = Text.isEmpty() || Entry.name_.contains( Text, Qt::CaseInsensitive ) || Entry.location_.contains( Text, Qt::CaseInsensitive );
            table_->setRowHidden( Row, !( Kind && Match ) );
            // A hidden row is never part of an action the user cannot see.
            if( !( Kind && Match ) && ( Check->flags() & Qt::ItemIsUserCheckable ) ) table_->item( Row, CheckColumn )->setCheckState( Qt::Unchecked );
            Shown += Kind && Match;
        }
        empty_->setText( items_.empty() ? tr( "보존된 복구 사본이 없습니다." ) : tr( "찾는 사본이 없습니다." ) );
        empty_->setVisible( Shown == 0 ); table_->setVisible( Shown > 0 );
        refreshButtons();
    }

    std::vector< int > RecoveryManagerDialog::checked() const
    {
        std::vector< int > Result;
        for( int Row = 0; Row < table_->rowCount(); ++Row ) {
            const auto* Check = table_->item( Row, CheckColumn );
            if( Check && !table_->isRowHidden( Row ) && Check->checkState() == Qt::Checked ) Result.push_back( Check->data( Qt::UserRole ).toInt() );
        }
        return Result;
    }

    void RecoveryManagerDialog::refreshButtons()
    {
        const auto Selected = busy_ ? std::vector< int >{} : checked();
        count_->setText( Selected.empty() ? QString{} : tr( "%1개 선택" ).arg( Selected.size() ) );
        open_->setEnabled( !Selected.empty() ); delete_->setEnabled( !Selected.empty() );
        open_->setText( Selected.size() > 1 ? tr( "%1개 별도 문서로 열기" ).arg( Selected.size() ) : tr( "별도 문서로 열기" ) );
        const bool Archive = Selected.size() == 1 && items_[ Selected.front() ].kind_ == recovery::Kind::Archive;
        apply_->setEnabled( Archive && items_[ Selected.front() ].applyBlocker_.isEmpty() );
        apply_->setToolTip( Archive && !items_[ Selected.front() ].applyBlocker_.isEmpty() ? ArchiveMessage( items_[ Selected.front() ].applyBlocker_ ) : QString{} );
        bool HotExit = false; for( const int Index : Selected ) HotExit = HotExit || items_[ Index ].state_ == recovery::State::HotExit;
        confirmText_->setText( tr( "선택한 사본 %1개를 삭제합니다. 되돌릴 수 없습니다." ).arg( Selected.size() )
            + ( HotExit ? QChar( ' ' ) + tr( "HotExit 대기 문서는 다음 실행의 이어 가기에서 빠집니다." ) : QString{} ) );
    }

    void RecoveryManagerDialog::showDetail( int Index )
    {
        if( Index < 0 || Index >= int( items_.size() ) ) { detail_->clear(); return; }
        current_ = Index; const auto& Entry = items_[ Index ];
        QString Text;
        if( Entry.state_ == recovery::State::InUse ) Text = tr( "열려 있는 창에서 쓰고 있는 사본입니다. 그 창에서 저장하거나 닫으면 정리됩니다. 열거나 삭제할 수 없습니다." );
        else if( Entry.state_ == recovery::State::HotExit ) Text = tr( "다음에 HyperEditor를 열면 이어서 편집할 문서입니다. 삭제하면 이어 가기에서 빠집니다." );
        else if( Entry.kind_ == recovery::Kind::Untitled ) Text = tr( "저장한 적 없는 문서입니다. 별도 문서로 연 뒤 저장할 위치를 정하십시오." );
        else if( Entry.kind_ == recovery::Kind::Edit ) Text = tr( "%1의 저장하지 않은 변경입니다. 별도 문서로 열어 확인한 뒤 원본에 저장할 수 있습니다." ).arg( Entry.name_ );
        else if( Entry.kind_ == recovery::Kind::Epub ) Text = tr( "%1의 편집 초안입니다. 원본 EPUB이 옮겨지거나 지워져도 보관된 판본으로 엽니다." ).arg( Entry.name_ );
        else {
            Text = tr( "%1의 %2을(를) 편집한 내용입니다. 압축 파일에 반영하면 압축 안의 파일을 이 내용으로 바꿉니다. 그 사이 압축 파일이 바뀌었으면 반영하지 않고 알려 줍니다." ).arg( Entry.location_, Entry.name_ );
            if( !Entry.applyBlocker_.isEmpty() ) Text += QChar( ' ' ) + ArchiveMessage( Entry.applyBlocker_ );
        }
        detail_->setText( Text );
    }

    void RecoveryManagerDialog::report( const QString& Text ) { detail_->setText( Text ); }
}
