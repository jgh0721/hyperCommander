#include "stdafx.h"
#include "newEpubDialog.hpp"

/// 모듈
#include "UniqueLibs/qtDialogFrame.hpp"

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리
#include <fmwidgets/Glyph.h>

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QRegularExpression>
#include <QVBoxLayout>

DECLARE_CMNLIBSV2_NAMESPACE

namespace hc::document
{
    namespace
    {
        const QList< QPair< QString, QString > > Languages{
            { "ko", QStringLiteral( "한국어" ) }, { "en", QStringLiteral( "English" ) }, { "ja", QStringLiteral( "日本語" ) },
            { "zh-Hans", QStringLiteral( "中文 简体" ) }, { "zh-Hant", QStringLiteral( "中文 繁體" ) },
            { "de", QStringLiteral( "Deutsch" ) }, { "fr", QStringLiteral( "Français" ) }, { "es", QStringLiteral( "Español" ) } };

        QString Size( qint64 Bytes )
        {
            return Bytes >= 1024 * 1024 ? QStringLiteral( "%1 MB" ).arg( Bytes / 1048576.0, 0, 'f', 2 ) : QStringLiteral( "%1 KB" ).arg( qMax< qint64 >( 1, Bytes / 1024 ) );
        }
    }

    QString NewEpubFileName( const QString& Title )
    {
        static const QRegularExpression Forbidden( QStringLiteral( "[\\\\/:*?\"<>|\\x00-\\x1f]" ) );
        auto Name = QString( Title ).replace( Forbidden, QStringLiteral( " " ) ).simplified();
        while( Name.endsWith( '.' ) || Name.endsWith( ' ' ) ) Name.chop( 1 );
        Name = Name.left( 120 ).trimmed();
        static const QRegularExpression Reserved( QStringLiteral( "^(con|prn|aux|nul|com[1-9]|lpt[1-9])$" ), QRegularExpression::CaseInsensitiveOption );
        if( Reserved.match( Name ).hasMatch() ) Name += '_';
        if( Name.isEmpty() ) Name = QObject::tr( "제목 없는 책" );
        return Name + QStringLiteral( ".epub" );
    }

    QString UniqueEpubFileName( const QString& Folder, const QString& FileName )
    {
        const QDir Directory( Folder );
        auto Base = QFileInfo( FileName ).completeBaseName();
        static const QRegularExpression Numbered( QStringLiteral( "^(.*) \\((\\d+)\\)$" ) );
        if( const auto Match = Numbered.match( Base ); Match.hasMatch() ) Base = Match.captured( 1 );
        for( int Number = 2; Number < 10000; ++Number ) {
            const auto Candidate = QStringLiteral( "%1 (%2).epub" ).arg( Base ).arg( Number );
            if( !QFileInfo::exists( Directory.filePath( Candidate ) ) ) return Candidate;
        }
        return FileName;
    }

    NewEpubDialog::NewEpubDialog( QWidget* Parent, const QString& Folder, const QString& Creator, const QString& Language )
        : QDialog( Parent )
    {
        setObjectName( "newEpubDialog" );
        setWindowTitle( tr( "새 EPUB 만들기" ) );
        setAttribute( Qt::WA_DeleteOnClose );
        auto* Form = new QFormLayout( this );
        Form->setFieldGrowthPolicy( QFormLayout::AllNonFixedFieldsGrow );
        title_ = new QLineEdit( tr( "제목 없는 책" ), this ); title_->setObjectName( "newEpub.title" ); title_->setMaxLength( 1024 );
        Form->addRow( tr( "제목(&T)" ), title_ );
        creator_ = new QLineEdit( Creator, this ); creator_->setObjectName( "newEpub.creator" ); creator_->setMaxLength( 1024 ); creator_->setPlaceholderText( tr( "선택 사항" ) );
        Form->addRow( tr( "저자(&A)" ), creator_ );
        language_ = new QComboBox( this ); language_->setObjectName( "newEpub.language" ); language_->setEditable( true );
        for( const auto& Item : Languages ) language_->addItem( QStringLiteral( "%1 · %2" ).arg( Item.first, Item.second ), Item.first );
        if( const auto Index = language_->findData( Language ); Index >= 0 ) language_->setCurrentIndex( Index ); else language_->setEditText( Language );
        language_->setToolTip( tr( "목록에 없으면 언어 코드를 직접 입력합니다. 예: ko, en-US" ) );
        Form->addRow( tr( "언어(&L)" ), language_ );
        chapter_ = new QLineEdit( tr( "1장" ), this ); chapter_->setObjectName( "newEpub.chapter" ); chapter_->setMaxLength( 1024 );
        Form->addRow( tr( "첫 장 제목(&C)" ), chapter_ );
        auto* CoverRow = new QHBoxLayout; CoverRow->setSpacing( 6 );
        cover_ = new QLabel( this ); cover_->setObjectName( "newEpub.cover" ); cover_->setTextFormat( Qt::PlainText ); cover_->setWordWrap( true );
        auto* Pick = new QPushButton( tr( "이미지 선택…" ), this ); Pick->setObjectName( "newEpub.coverPick" );
        coverClear_ = new QPushButton( tr( "지우기" ), this ); coverClear_->setObjectName( "newEpub.coverClear" );
        CoverRow->addWidget( cover_, 1 ); CoverRow->addWidget( Pick ); CoverRow->addWidget( coverClear_ );
        Form->addRow( tr( "표지" ), CoverRow );
        stylesheet_ = new QCheckBox( tr( "기본 스타일시트 포함 — 줄 간격 1.8 · 단어 단위 줄바꿈 · 문단 들여쓰기" ), this );
        stylesheet_->setObjectName( "newEpub.stylesheet" ); stylesheet_->setChecked( true );
        Form->addRow( QString{}, stylesheet_ );
        auto* FolderRow = new QHBoxLayout; FolderRow->setSpacing( 6 );
        folder_ = new QLineEdit( QDir::toNativeSeparators( Folder ), this ); folder_->setObjectName( "newEpub.folder" );
        auto* Browse = new QPushButton( tr( "찾아보기…" ), this ); Browse->setObjectName( "newEpub.browse" );
        FolderRow->addWidget( folder_, 1 ); FolderRow->addWidget( Browse );
        Form->addRow( tr( "저장 위치(&F)" ), FolderRow );
        file_ = new QLineEdit( NewEpubFileName( title_->text() ), this ); file_->setObjectName( "newEpub.file" ); file_->setMaxLength( 200 );
        Form->addRow( tr( "파일 이름(&N)" ), file_ );
        auto* MessageRow = new QHBoxLayout; MessageRow->setSpacing( 6 );
        message_ = new QLabel( this ); message_->setObjectName( "newEpub.message" ); message_->setWordWrap( true ); message_->setTextFormat( Qt::PlainText );
        suggest_ = new QPushButton( this ); suggest_->setObjectName( "newEpub.suggest" ); suggest_->hide();
        MessageRow->addWidget( message_, 1 ); MessageRow->addWidget( suggest_ );
        Form->addRow( MessageRow );
        auto* Buttons = new QDialogButtonBox( QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this );
        create_ = Buttons->button( QDialogButtonBox::Ok ); create_->setText( tr( "만들고 편집" ) ); create_->setObjectName( "newEpub.create" );
        Buttons->button( QDialogButtonBox::Cancel )->setText( tr( "취소" ) );
        Form->addRow( Buttons );
        connect( Buttons, &QDialogButtonBox::accepted, this, &NewEpubDialog::accept );
        connect( Buttons, &QDialogButtonBox::rejected, this, &NewEpubDialog::reject );
        hc::ui::ApplyDialogFrame( this, Buttons, { {}, tr( "빈 책을 만들고 EPUB 편집 모드로 엽니다. 책 정보는 나중에 ‘책 정보’에서 바꿀 수 있습니다." ), fm::ui::glyph::NewFile } );
        resize( 600, sizeHint().height() );

        connect( title_, &QLineEdit::textEdited, this, [ this ]( const QString& Text ) { if( !fileEdited_ ) file_->setText( NewEpubFileName( Text ) ); validate(); } );
        connect( file_, &QLineEdit::textEdited, this, [ this ] { fileEdited_ = true; restoredError_.clear(); validate(); } );
        for( auto* Edit : { creator_, chapter_, folder_ } ) connect( Edit, &QLineEdit::textChanged, this, [ this ] { restoredError_.clear(); validate(); } );
        connect( language_, &QComboBox::currentTextChanged, this, &NewEpubDialog::validate );
        connect( suggest_, &QPushButton::clicked, this, [ this ] {
            file_->setText( UniqueEpubFileName( folder(), file_->text().trimmed() ) ); fileEdited_ = true; restoredError_.clear(); restoredExists_ = false; validate();
        } );
        connect( Pick, &QPushButton::clicked, this, &NewEpubDialog::pickCover );
        connect( coverClear_, &QPushButton::clicked, this, [ this ] { setCover( {} ); } );
        connect( Browse, &QPushButton::clicked, this, [ this ] {
            // Non-blocking: the hosted viewer's pump and this dialog keep running.
            auto* Dialog = new QFileDialog( this, tr( "저장 위치" ), folder() );
            Dialog->setFileMode( QFileDialog::Directory ); Dialog->setOption( QFileDialog::ShowDirsOnly ); Dialog->setAttribute( Qt::WA_DeleteOnClose );
            connect( Dialog, &QFileDialog::fileSelected, this, [ this ]( const QString& Path ) { folder_->setText( QDir::toNativeSeparators( Path ) ); } );
            Dialog->open();
        } );
        setCover( {} );
        title_->selectAll(); title_->setFocus();
    }

    QString NewEpubDialog::language() const
    {
        const auto Index = language_->findText( language_->currentText() );
        return Index >= 0 ? language_->itemData( Index ).toString() : language_->currentText().section( ' ', 0, 0 ).trimmed();
    }

    QString NewEpubDialog::folder() const { return QDir::fromNativeSeparators( folder_->text().trimmed() ); }

    void NewEpubDialog::pickCover()
    {
        auto* Dialog = new QFileDialog( this, tr( "표지 이미지" ), coverPath_.isEmpty() ? folder() : QFileInfo( coverPath_ ).absolutePath(),
            tr( "이미지 (*.png *.jpg *.jpeg *.gif *.webp)" ) );
        Dialog->setFileMode( QFileDialog::ExistingFile ); Dialog->setAttribute( Qt::WA_DeleteOnClose );
        connect( Dialog, &QFileDialog::fileSelected, this, &NewEpubDialog::setCover );
        Dialog->open();
    }

    void NewEpubDialog::setCover( const QString& Path )
    {
        coverPath_ = Path; coverBytes_.clear(); coverError_.clear();
        if( Path.isEmpty() ) {
            cover_->setText( tr( "없음 · PNG · JPEG · GIF · WebP, 8 MB · 4096 px 이하" ) );
            coverClear_->setEnabled( false ); validate(); return;
        }
        QFile File( Path );
        if( File.size() > epub::ImageByteLimit ) coverError_ = tr( "표지 이미지는 8 MB 이하여야 합니다." );
        else if( !File.open( QIODevice::ReadOnly ) ) coverError_ = tr( "표지 이미지를 읽지 못했습니다." );
        else {
            coverBytes_ = File.readAll();
            if( epub::ImageMediaType( coverBytes_ ).isEmpty() ) { coverBytes_.clear(); coverError_ = tr( "표지로 쓸 수 없는 이미지입니다. PNG · JPEG · GIF · WebP, 4096 px · 4 MP 이하만 받습니다." ); }
        }
        if( coverError_.isEmpty() ) {
            QBuffer Buffer( &coverBytes_ ); Buffer.open( QIODevice::ReadOnly ); const auto Dimensions = QImageReader( &Buffer ).size();
            cover_->setText( QStringLiteral( "%1 · %2 × %3 · %4" ).arg( QFileInfo( Path ).fileName() ).arg( Dimensions.width() ).arg( Dimensions.height() ).arg( Size( coverBytes_.size() ) ) );
        } else cover_->setText( QFileInfo( Path ).fileName() );
        coverClear_->setEnabled( true ); validate();
    }

    NewEpubRequest NewEpubDialog::Request() const
    {
        NewEpubRequest Result;
        Result.book_.title_ = title_->text().trimmed(); Result.book_.creator_ = creator_->text().trimmed();
        Result.book_.language_ = language(); Result.book_.chapterTitle_ = chapter_->text().trimmed();
        Result.book_.stylesheet_ = stylesheet_->isChecked(); Result.book_.coverImage_ = coverBytes_;
        Result.path_ = QDir( folder() ).filePath( file_->text().trimmed() ); Result.coverPath_ = coverPath_;
        return Result;
    }

    void NewEpubDialog::Restore( const NewEpubRequest& Request, const QString& Error, bool Exists )
    {
        title_->setText( Request.book_.title_ ); creator_->setText( Request.book_.creator_ );
        if( const auto Index = language_->findData( Request.book_.language_ ); Index >= 0 ) language_->setCurrentIndex( Index ); else language_->setEditText( Request.book_.language_ );
        chapter_->setText( Request.book_.chapterTitle_ ); stylesheet_->setChecked( Request.book_.stylesheet_ );
        folder_->setText( QDir::toNativeSeparators( QFileInfo( Request.path_ ).absolutePath() ) );
        file_->setText( QFileInfo( Request.path_ ).fileName() ); fileEdited_ = true;
        setCover( Request.coverPath_ );
        restoredError_ = Error; restoredExists_ = Exists;
        validate();
    }

    void NewEpubDialog::validate()
    {
        QString Problem, Suggestion;
        const auto Folder = folder(), File = file_->text().trimmed();
        static const QRegularExpression Forbidden( QStringLiteral( "[\\\\/:*?\"<>|\\x00-\\x1f]" ) );
        if( !epub::ValidBookText( title_->text(), true ) ) Problem = tr( "제목을 입력하세요. 제어 문자는 쓸 수 없습니다." );
        else if( !epub::ValidBookText( creator_->text(), false ) ) Problem = tr( "저자에 제어 문자는 쓸 수 없습니다." );
        else if( !epub::ValidLanguageTag( language() ) ) Problem = tr( "언어 코드가 올바르지 않습니다. 예: ko, en-US" );
        else if( !epub::ValidBookText( chapter_->text(), true ) ) Problem = tr( "첫 장 제목을 입력하세요." );
        else if( !coverError_.isEmpty() ) Problem = coverError_;
        else if( Folder.isEmpty() || !QFileInfo( Folder ).isAbsolute() || !QFileInfo( Folder ).isDir() ) Problem = tr( "저장 위치 폴더를 찾을 수 없습니다." );
        else if( !QFileInfo( Folder ).isWritable() ) Problem = tr( "이 폴더에 쓸 수 없습니다." );
        else if( File.size() <= 5 || !File.endsWith( QStringLiteral( ".epub" ), Qt::CaseInsensitive ) ) Problem = tr( "파일 이름은 .epub으로 끝나야 합니다." );
        else if( File.contains( Forbidden ) ) Problem = tr( "파일 이름에 \\ / : * ? \" < > | 는 쓸 수 없습니다." );
        else if( restoredExists_ || QFileInfo::exists( QDir( Folder ).filePath( File ) ) ) {
            Problem = tr( "‘%1’이(가) 이미 있습니다. 덮어쓰지 않습니다." ).arg( File );
            Suggestion = UniqueEpubFileName( Folder, File );
        } else if( !restoredError_.isEmpty() ) Problem = restoredError_;
        create_->setEnabled( Problem.isEmpty() || ( !restoredError_.isEmpty() && Problem == restoredError_ ) );
        if( Problem.isEmpty() ) message_->setText( tr( "%1 을(를) 만들고 EPUB 편집 모드로 엽니다." ).arg( QDir::toNativeSeparators( QDir( Folder ).filePath( File ) ) ) );
        else message_->setText( Problem );
        auto Palette = message_->palette();
        Palette.setColor( QPalette::WindowText, fm::style::themeColorsFor( message_ )[ Problem.isEmpty() ? fm::style::Token::Fg2 : fm::style::Token::Danger ] );
        message_->setPalette( Palette );
        suggest_->setVisible( !Suggestion.isEmpty() );
        if( !Suggestion.isEmpty() ) suggest_->setText( tr( "‘%1’(으)로 바꾸기" ).arg( Suggestion ) );
    }

    void NewEpubDialog::accept()
    {
        restoredExists_ = false; validate();
        if( !create_->isEnabled() ) return;
        const auto Value = Request();
        // D5: the file is created after the dialog closes; a later write failure reopens it with these values.
        QDialog::accept();
        emit Requested( Value );
    }
}
