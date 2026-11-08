#include "stdafx.h"
#include "epubRestoreConflictDialog.hpp"

/// 모듈
#include "UniqueLibs/qtDialogFrame.hpp"
#include "UniqueLibs/qtMessageDialog.hpp"

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리
#include "commonLibs/document/epub/epubDraftStore.hpp"
#include "commonLibs/document/epub/epubEditModel.hpp"

/// 외부 라이브러리
#include <fmwidgets/Glyph.h>

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QCryptographicHash>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QRegularExpression>
#include <QVBoxLayout>
#include <tuple>

DECLARE_CMNLIBSV2_NAMESPACE

namespace hc::document
{
    namespace
    {
        EpubBookFacts Describe( const epub::EditModel& Model, qint64 Size )
        {
            EpubBookFacts Facts; Facts.readable_ = true; Facts.size_ = Size;
            const auto Values = Model.Metadata()[ "values" ].toObject();
            Facts.title_ = Values[ "title" ].toString(); Facts.creator_ = Values[ "creator" ].toString();
            Facts.publisher_ = Values[ "publisher" ].toString(); Facts.language_ = Values[ "language" ].toString();
            Facts.chapters_ = int( Model.Publication()[ "spine" ].toArray().size() );
            static const QRegularExpression Modified( QStringLiteral( "property\\s*=\\s*\"dcterms:modified\"[^>]*>\\s*([^<]+?)\\s*<" ) );
            const auto Package = QString::fromUtf8( Model.Read( Model.Publication()[ "rootfile" ].toString() ) );
            if( const auto Match = Modified.match( Package ); Match.hasMatch() ) Facts.modified_ = Match.captured( 1 );
            return Facts;
        }
        QString Size( qint64 Bytes )
        {
            if( Bytes < 0 ) return QStringLiteral( "—" );
            return Bytes >= 1024 * 1024 ? QStringLiteral( "%1 MB" ).arg( Bytes / 1048576.0, 0, 'f', 2 ) : QStringLiteral( "%1 KB" ).arg( qMax< qint64 >( 1, Bytes / 1024 ) );
        }
        QString When( const QDateTime& Time )
        {
            return Time.isValid() ? QLocale().toString( Time.toLocalTime(), QStringLiteral( "yyyy-MM-dd HH:mm" ) ) : QStringLiteral( "—" );
        }
    }

    EpubRestoreFacts GatherEpubRestoreFacts( const QString& DraftRoot, const QString& DraftPath, const QString& BookPath )
    {
        EpubRestoreFacts Facts; Facts.bookPath_ = BookPath; Facts.draftPath_ = DraftPath;
        const auto Draft = epub::DraftStore::Read( DraftRoot, DraftPath );
        if( !Draft ) { Facts.error_ = Draft.error_; return Facts; }
        Facts.lastEdit_ = QFileInfo( DraftPath ).lastModified();
        epub::EditModel Original;
        if( Original.Load( Draft.snapshot_.original_ ) ) Facts.original_ = Describe( Original, Draft.snapshot_.original_.size() );
        Facts.changed_ = Draft.snapshot_.changes_.keys();
        if( !Draft.snapshot_.structuralBytes_.isEmpty() && Original.Publication().contains( "rootfile" ) ) {
            const auto Package = Original.Publication()[ "rootfile" ].toString();
            if( !Facts.changed_.contains( Package ) ) Facts.changed_.append( Package );
        }
        QFile File( BookPath );
        if( !File.exists() ) { Facts.missing_ = true; Facts.sourceChanged_ = true; return Facts; }
        if( File.size() > epub::InputLimit || !File.open( QIODevice::ReadOnly ) ) { Facts.sourceChanged_ = true; Facts.current_.size_ = File.size(); return Facts; }
        const auto Bytes = File.readAll();
        Facts.sourceChanged_ = QCryptographicHash::hash( Bytes, QCryptographicHash::Sha256 ) != Draft.snapshot_.diskHash_;
        if( !Facts.sourceChanged_ ) return Facts;
        epub::EditModel Current;
        if( Current.Load( Bytes ) ) {
            Facts.current_ = Describe( Current, Bytes.size() );
            for( const auto& Path : Facts.changed_ ) if( Original.Read( Path ) != Current.Read( Path ) ) Facts.alsoChanged_.append( Path );
        } else Facts.current_.size_ = Bytes.size();
        Facts.current_.fileModified_ = QFileInfo( BookPath ).lastModified();
        return Facts;
    }

    EpubRestoreConflictDialog::EpubRestoreConflictDialog( const EpubRestoreFacts& Facts, QWidget* Parent )
        : QDialog( Parent )
    {
        setObjectName( "epubRestoreConflict" );
        setAttribute( Qt::WA_DeleteOnClose );
        const auto Name = QFileInfo( Facts.bookPath_ ).fileName();
        setWindowTitle( tr( "편집 내용 복원" ) );
        auto* Layout = new QVBoxLayout( this ); Layout->setSpacing( 10 );
        auto* Path = new QLabel( QDir::toNativeSeparators( Facts.bookPath_ ), this ); Path->setTextFormat( Qt::PlainText ); Path->setObjectName( "epubRestoreConflict.path" );
        Layout->addWidget( Path );

        auto* Grid = new QGridLayout; Grid->setHorizontalSpacing( 16 ); Grid->setVerticalSpacing( 4 );
        const auto Header = [ this, Grid ]( int Column, const QString& Text ) {
            auto* Label = new QLabel( Text, this ); auto Font = Label->font(); Font.setWeight( QFont::DemiBold ); Label->setFont( Font ); Grid->addWidget( Label, 0, Column );
        };
        Header( 0, tr( "항목" ) ); Header( 1, tr( "편집을 시작한 원본" ) ); Header( 2, tr( "지금 파일" ) );
        const auto Missing = tr( "찾을 수 없음" );
        const auto Cell = [ &Facts, &Missing ]( const EpubBookFacts& Side, bool Current, const QString& Value ) {
            if( Current && Facts.missing_ ) return Missing;
            return Side.readable_ || Current ? ( Value.isEmpty() ? QStringLiteral( "—" ) : Value ) : QStringLiteral( "—" );
        };
        const auto Delta = Facts.current_.size_ >= 0 && Facts.original_.size_ >= 0 && !Facts.missing_
            ? QStringLiteral( " (%1%2)" ).arg( Facts.current_.size_ >= Facts.original_.size_ ? QStringLiteral( "+" ) : QStringLiteral( "−" ) ).arg( Size( qAbs( Facts.current_.size_ - Facts.original_.size_ ) ) )
            : QString{};
        const QList< std::tuple< QString, QString, QString > > Rows{
            { tr( "제목" ), Cell( Facts.original_, false, Facts.original_.title_ ), Cell( Facts.current_, true, Facts.current_.title_ ) },
            { tr( "저자" ), Cell( Facts.original_, false, Facts.original_.creator_ ), Cell( Facts.current_, true, Facts.current_.creator_ ) },
            { tr( "출판사" ), Cell( Facts.original_, false, Facts.original_.publisher_ ), Cell( Facts.current_, true, Facts.current_.publisher_ ) },
            { tr( "언어" ), Cell( Facts.original_, false, Facts.original_.language_ ), Cell( Facts.current_, true, Facts.current_.language_ ) },
            { tr( "책 수정일" ), Cell( Facts.original_, false, Facts.original_.modified_ ), Cell( Facts.current_, true, Facts.current_.modified_ ) },
            { tr( "장 수" ), Cell( Facts.original_, false, Facts.original_.readable_ ? QString::number( Facts.original_.chapters_ ) : QString{} ),
                Cell( Facts.current_, true, Facts.current_.readable_ ? QString::number( Facts.current_.chapters_ ) : QString{} ) },
            { tr( "파일 크기" ), Size( Facts.original_.size_ ), Facts.missing_ ? Missing : Size( Facts.current_.size_ ) + Delta },
            { tr( "파일 수정 시각" ), QStringLiteral( "—" ), Facts.missing_ ? Missing : When( Facts.current_.fileModified_ ) } };
        int Row = 1;
        for( const auto& [ Key, Left, Right ] : Rows ) {
            auto* KeyLabel = new QLabel( Key, this ), *LeftLabel = new QLabel( Left, this ), *RightLabel = new QLabel( Right, this );
            for( auto* Label : { KeyLabel, LeftLabel, RightLabel } ) Label->setTextFormat( Qt::PlainText );
            // Rows that differ stand out (warning colour, bold value).
            const bool Different = !Facts.missing_ && Left != QStringLiteral( "—" ) && Left != Right.section( QStringLiteral( " (" ), 0, 0 );
            if( Different ) for( auto* Label : { KeyLabel, LeftLabel, RightLabel } ) {
                auto Palette = Label->palette(); Palette.setColor( QPalette::WindowText, fm::style::themeColorsFor( Label )[ fm::style::Token::Warn ] ); Label->setPalette( Palette );
                auto Font = Label->font(); Font.setWeight( QFont::DemiBold ); Label->setFont( Font );
            }
            Grid->addWidget( KeyLabel, Row, 0 ); Grid->addWidget( LeftLabel, Row, 1 ); Grid->addWidget( RightLabel, Row, 2 ); ++Row;
        }
        Grid->setColumnStretch( 1, 1 ); Grid->setColumnStretch( 2, 1 );
        Layout->addLayout( Grid );

        QStringList Names; for( const auto& Item : Facts.changed_.mid( 0, 3 ) ) Names << QFileInfo( Item ).fileName();
        auto Changes = tr( "편집하던 내용 · 바뀐 리소스 %1개: %2" ).arg( Facts.changed_.size() )
            .arg( Names.join( QStringLiteral( ", " ) ) + ( Facts.changed_.size() > 3 ? tr( " 외 %1개" ).arg( Facts.changed_.size() - 3 ) : QString{} ) );
        Changes += '\n' + tr( "마지막 편집 %1" ).arg( When( Facts.lastEdit_ ) );
        if( !Facts.alsoChanged_.isEmpty() ) {
            QStringList Also; for( const auto& Item : Facts.alsoChanged_ ) Also << QFileInfo( Item ).fileName();
            Changes += '\n' + tr( "이 중 %1개(%2)는 지금 파일에서도 바뀌었습니다." ).arg( Facts.alsoChanged_.size() ).arg( Also.join( QStringLiteral( ", " ) ) );
        }
        auto* Summary = new QLabel( Changes, this ); Summary->setObjectName( "epubRestoreConflict.changes" ); Summary->setTextFormat( Qt::PlainText ); Summary->setWordWrap( true );
        Layout->addWidget( Summary );

        auto* Buttons = new QDialogButtonBox( this );
        auto* Copy = Buttons->addButton( tr( "편집하던 내용을 사본으로 열기" ), QDialogButtonBox::AcceptRole ); Copy->setObjectName( "epubRestoreConflict.copy" );
        auto* Other = Buttons->addButton( Facts.missing_ ? tr( "나중에(초안 보관)" ) : tr( "지금 파일 열기" ), QDialogButtonBox::RejectRole ); Other->setObjectName( "epubRestoreConflict.current" );
        connect( Copy, &QPushButton::clicked, this, [ this ] { finish( OpenEditsAsCopy ); } );
        connect( Other, &QPushButton::clicked, this, [ this, Missing = Facts.missing_ ] { finish( Missing ? Later : OpenCurrent ); } );
        auto* Footer = hc::ui::ApplyDialogFrame( this, Buttons, {
            Facts.missing_ ? tr( "‘%1’ 원본을 찾을 수 없습니다" ).arg( Name ) : tr( "‘%1’의 원본이 바뀌었습니다" ).arg( Name ),
            tr( "닫기 전에 편집하던 내용은 보관되어 있습니다. 원본 파일은 고치지 않았습니다. 어떻게 열지 고르세요." ), fm::ui::glyph::Clock } );
        if( !Facts.missing_ ) {
            auto* Discard = new QPushButton( tr( "초안 버리고 지금 파일 열기…" ), this ); Discard->setObjectName( "epubRestoreConflict.discard" );
            Footer->insertWidget( 0, Discard ); Footer->insertStretch( 1, 1 );
            connect( Discard, &QPushButton::clicked, this, [ this ] {
                if( hc::ui::MessageDialog::question( this, tr( "초안 버리기" ), tr( "편집하던 내용을 지우고 지금 파일을 엽니다. 되돌릴 수 없습니다. 계속하시겠습니까?" ),
                        QMessageBox::Yes | QMessageBox::Cancel ) == QMessageBox::Yes ) finish( DiscardDraft );
            } );
        }
        // Closing the window keeps everything: the same as "later" (the draft stays in the EPUB draft list).
        connect( this, &QDialog::rejected, this, [ this, Missing = Facts.missing_ ] { finish( Missing ? Later : OpenCurrent ); } );
        resize( 640, sizeHint().height() );
    }

    void EpubRestoreConflictDialog::finish( Choice Value )
    {
        if( chosen_ ) return;
        chosen_ = true;
        emit Chosen( int( Value ) );
        if( isVisible() ) done( Value == OpenEditsAsCopy ? QDialog::Accepted : QDialog::Rejected );
    }
}
