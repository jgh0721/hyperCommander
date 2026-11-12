#include "stdafx.h"
#include "rstProjectDialogs.hpp"

/// 모듈
#include "UniqueLibs/qtDialogFrame.hpp"
#include "rst/rstSettings.hpp"
#include "rst/sphinxBuilders.hpp"

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리
#include <fmstyle/ThemeManager.h>
#include <fmwidgets/Glyph.h>

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDate>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

DECLARE_CMNLIBSV2_NAMESPACE

namespace hc::document
{
    namespace
    {
        using fm::style::Token;

        /// 파이썬 작은따옴표 문자열 리터럴.
        QString Py( const QString& Text )
        {
            QString Out = Text;
            Out.replace( QLatin1Char( '\\' ), QStringLiteral( "\\\\" ) ).replace( QLatin1Char( '\'' ), QStringLiteral( "\\'" ) );
            Out.remove( QLatin1Char( '\n' ) ).remove( QLatin1Char( '\r' ) );
            return QLatin1Char( '\'' ) + Out + QLatin1Char( '\'' );
        }
        /// docutils 가 제목 밑줄 길이를 재는 방식: 넓은 글자( 한글 · 한자 · 가나 · 전각 )는 두 칸이다.
        int DisplayWidth( const QString& Text )
        {
            int Width = 0;
            for( const uint Code : Text.toUcs4() ) {
                const bool Wide = ( Code >= 0x1100 && Code <= 0x115F ) || ( Code >= 0x2E80 && Code <= 0xA4CF ) || ( Code >= 0xAC00 && Code <= 0xD7A3 )
                               || ( Code >= 0xF900 && Code <= 0xFAFF ) || ( Code >= 0xFE30 && Code <= 0xFE4F ) || ( Code >= 0xFF00 && Code <= 0xFF60 )
                               || ( Code >= 0xFFE0 && Code <= 0xFFE6 ) || ( Code >= 0x20000 && Code <= 0x3FFFD );
                Width += Wide ? 2 : 1;
            }
            return Width;
        }
        bool ValidFolderName( const QString& Name )
        {
            static const QString Forbidden = QStringLiteral( "<>:\"/\\|?*" );
            if( Name.isEmpty() || Name == QLatin1String( "." ) || Name == QLatin1String( ".." ) || Name.endsWith( QLatin1Char( ' ' ) ) || Name.endsWith( QLatin1Char( '.' ) ) ) return false;
            for( const QChar Ch : Name ) if( Ch.unicode() < 0x20 || Forbidden.contains( Ch ) ) return false;
            return true;
        }
        bool WriteText( const QString& Path, const QString& Text )
        {
            // 파이썬과 reST 원본의 관례대로 UTF-8( BOM 없음 ), LF.
            QSaveFile File( Path );
            return File.open( QIODevice::WriteOnly ) && File.write( Text.toUtf8() ) >= 0 && File.commit();
        }
        void Tint( QLabel* Label, const Token Which )
        {
            auto Palette = Label->palette();
            Palette.setColor( QPalette::WindowText, fm::style::themeColorsFor( Label )[ Which ] );
            Label->setPalette( Palette );
        }
        QPlainTextEdit* SourcePreview( QWidget* Parent, const char* Name )
        {
            auto* Edit = new QPlainTextEdit( Parent );
            Edit->setObjectName( Name ); Edit->setReadOnly( true ); Edit->setLineWrapMode( QPlainTextEdit::NoWrap );
            auto Font = QFontDatabase::systemFont( QFontDatabase::FixedFont ); Font.setPointSizeF( qMax( 9.0, Font.pointSizeF() ) );
            Edit->setFont( Font ); Edit->setFrameShape( QFrame::NoFrame );
            return Edit;
        }
    }

    // ── 새 Sphinx 프로젝트: 내용 ─────────────────────────────────────────

    QString SphinxProjectSpec::Root() const { return QDir( directory_ ).filePath( folder_ ); }
    QString SphinxProjectSpec::SourceDirectory() const { return separate_ ? QDir( Root() ).filePath( QStringLiteral( "source" ) ) : Root(); }

    QString SphinxProjectSpec::Error() const
    {
        if( name_.trimmed().isEmpty() ) return QObject::tr( "프로젝트 이름을 입력하세요." );
        if( directory_.trimmed().isEmpty() || !QDir::isAbsolutePath( directory_ ) ) return QObject::tr( "프로젝트를 둘 위치를 지정하세요." );
        if( !ValidFolderName( folder_ ) ) return QObject::tr( "폴더 이름으로 쓸 수 없는 글자가 있습니다." );
        if( !QFileInfo( directory_ ).isDir() ) return QObject::tr( "위치를 찾을 수 없습니다: %1" ).arg( QDir::toNativeSeparators( directory_ ) );
        const QDir Source( SourceDirectory() );
        if( QFileInfo::exists( Source.filePath( QStringLiteral( "conf.py" ) ) ) ) return QObject::tr( "이 폴더에는 이미 Sphinx 프로젝트(conf.py)가 있습니다." );
        if( QFileInfo::exists( Source.filePath( QStringLiteral( "index.rst" ) ) ) ) return QObject::tr( "이 폴더에는 이미 index.rst가 있습니다. 덮어쓰지 않습니다." );
        if( QFileInfo( Root() ).isFile() ) return QObject::tr( "같은 이름의 파일이 있습니다: %1" ).arg( QDir::toNativeSeparators( Root() ) );
        return {};
    }

    QString SphinxFolderName( const QString& ProjectName )
    {
        QString Out;
        for( const QChar Ch : ProjectName.trimmed().toLower() ) {
            if( Ch.unicode() < 0x80 && Ch.isLetterOrNumber() ) Out += Ch;
            else if( !Out.isEmpty() && !Out.endsWith( QLatin1Char( '-' ) ) ) Out += QLatin1Char( '-' );
        }
        while( Out.endsWith( QLatin1Char( '-' ) ) ) Out.chop( 1 );
        return Out.isEmpty() ? QStringLiteral( "docs" ) : Out;
    }

    QStringList SphinxThemeNames()
    {
        return { "alabaster", "furo", "sphinx_rtd_theme", "pydata_sphinx_theme", "sphinx_book_theme", "shibuya", "piccolo_theme", "sphinxawesome_theme",
                 "classic", "sphinxdoc", "nature", "bizstyle" };
    }

    QString SphinxConfText( const SphinxProjectSpec& Spec )
    {
        const QString Author = Spec.author_.trimmed();
        QStringList Lines{
            QStringLiteral( "# Sphinx 설정. 항목 설명: https://www.sphinx-doc.org/en/master/usage/configuration.html" ),
            QString(),
            QStringLiteral( "project = %1" ).arg( Py( Spec.name_.trimmed() ) ),
            QStringLiteral( "author = %1" ).arg( Py( Author ) ),
            QStringLiteral( "copyright = %1" ).arg( Py( Author.isEmpty() ? QString::number( QDate::currentDate().year() )
                                                                       : QStringLiteral( "%1, %2" ).arg( QDate::currentDate().year() ).arg( Author ) ) ),
            QStringLiteral( "release = '0.1'" ),
            QString(),
            Spec.myst_ ? QStringLiteral( "extensions = ['myst_parser']" ) : QStringLiteral( "extensions = []" ) };
        if( Spec.myst_ ) Lines << QStringLiteral( "source_suffix = {'.rst': 'restructuredtext', '.md': 'markdown'}" );
        Lines << QStringLiteral( "templates_path = ['_templates']" )
              << QStringLiteral( "exclude_patterns = ['_build', 'Thumbs.db', '.DS_Store']" )
              << QStringLiteral( "language = %1" ).arg( Py( Spec.language_ ) )
              << QString()
              << QStringLiteral( "html_theme = %1" ).arg( Py( Spec.theme_ ) )
              << QStringLiteral( "html_static_path = ['_static']" )
              << QString();
        return Lines.join( QLatin1Char( '\n' ) );
    }

    QString SphinxIndexText( const SphinxProjectSpec& Spec )
    {
        const QString Title = Spec.name_.trimmed().isEmpty() ? QObject::tr( "프로젝트" ) : Spec.name_.trimmed();
        return QStringList{ Title, QString( DisplayWidth( Title ), QLatin1Char( '=' ) ), QString(),
                            QStringLiteral( ".. toctree::" ), QStringLiteral( "   :maxdepth: 2" ),
                            QStringLiteral( "   :caption: %1" ).arg( Spec.language_ == QLatin1String( "ko" ) ? QStringLiteral( "차례" )
                                                                 : Spec.language_ == QLatin1String( "ja" ) ? QStringLiteral( "目次" ) : QStringLiteral( "Contents" ) ),
                            QString(), QString(),
                            Spec.language_ == QLatin1String( "ko" ) ? QStringLiteral( "여기에 첫 문단을 씁니다." )
                            : Spec.language_ == QLatin1String( "ja" ) ? QStringLiteral( "ここに最初の段落を書きます。" ) : QStringLiteral( "Write the first paragraph here." ),
                            QString() }.join( QLatin1Char( '\n' ) );
    }

    QString CreateSphinxProject( const SphinxProjectSpec& Spec, QString* IndexPath )
    {
        if( const auto Error = Spec.Error(); !Error.isEmpty() ) return Error;
        const QDir Source( Spec.SourceDirectory() );
        QStringList Folders{ Source.absolutePath(), Source.filePath( QStringLiteral( "_static" ) ), Source.filePath( QStringLiteral( "_templates" ) ) };
        if( Spec.separate_ ) Folders << QDir( Spec.Root() ).filePath( QStringLiteral( "build" ) );
        for( const auto& Folder : Folders )
            if( !QDir().mkpath( Folder ) ) return QObject::tr( "폴더를 만들 수 없습니다: %1" ).arg( QDir::toNativeSeparators( Folder ) );
        const QString Conf = Source.filePath( QStringLiteral( "conf.py" ) ), Index = Source.filePath( QStringLiteral( "index.rst" ) );
        if( !WriteText( Conf, SphinxConfText( Spec ) ) ) return QObject::tr( "파일을 쓸 수 없습니다: %1" ).arg( QDir::toNativeSeparators( Conf ) );
        if( !WriteText( Index, SphinxIndexText( Spec ) ) ) return QObject::tr( "파일을 쓸 수 없습니다: %1" ).arg( QDir::toNativeSeparators( Index ) );
        if( IndexPath ) *IndexPath = Index;
        return {};
    }

    // ── 새 Sphinx 프로젝트: 대화상자 ─────────────────────────────────────

    NewSphinxProjectDialog::NewSphinxProjectDialog( QWidget* Parent, const Options& Value )
        : QDialog( Parent ), options_( Value )
    {
        setObjectName( "newSphinxProjectDialog" );
        setWindowTitle( tr( "새 Sphinx 프로젝트" ) );
        setAttribute( Qt::WA_DeleteOnClose );
        auto* Layout = new QVBoxLayout( this );
        auto* Columns = new QHBoxLayout; Columns->setSpacing( 16 );
        Layout->addLayout( Columns, 1 );

        // 왼쪽: 입력.
        auto* Form = new QFormLayout; Form->setFieldGrowthPolicy( QFormLayout::AllNonFixedFieldsGrow );
        name_ = new QLineEdit( this ); name_->setObjectName( "newSphinx.name" ); name_->setMaxLength( 200 );
        author_ = new QLineEdit( options_.author_, this ); author_->setObjectName( "newSphinx.author" ); author_->setMaxLength( 200 );
        directory_ = new QLineEdit( QDir::toNativeSeparators( options_.directory_ ), this ); directory_->setObjectName( "newSphinx.directory" );
        auto* Browse = new QPushButton( tr( "찾기…" ), this ); Browse->setObjectName( "newSphinx.browse" );
        auto* Location = new QHBoxLayout; Location->setSpacing( 6 ); Location->addWidget( directory_, 1 ); Location->addWidget( Browse );
        folder_ = new QLineEdit( this ); folder_->setObjectName( "newSphinx.folder" ); folder_->setMaxLength( 120 );
        language_ = new QComboBox( this ); language_->setObjectName( "newSphinx.language" );
        language_->addItem( tr( "한국어 (ko)" ), "ko" ); language_->addItem( QStringLiteral( "English (en)" ), "en" ); language_->addItem( QStringLiteral( "日本語 (ja)" ), "ja" );
        theme_ = new QComboBox( this ); theme_->setObjectName( "newSphinx.theme" );
        for( const auto& Theme : SphinxThemeNames() ) theme_->addItem( Theme == QLatin1String( "alabaster" ) ? tr( "alabaster (Sphinx 기본값)" ) : Theme, Theme );
        flat_ = new QRadioButton( tr( "문서와 conf.py를 한 폴더에" ), this ); flat_->setObjectName( "newSphinx.flat" ); flat_->setChecked( true );
        separate_ = new QRadioButton( tr( "source · build 폴더로 분리" ), this ); separate_->setObjectName( "newSphinx.separate" );
        auto* Structure = new QVBoxLayout; Structure->setSpacing( 4 ); Structure->addWidget( flat_ ); Structure->addWidget( separate_ );
        myst_ = new QCheckBox( tr( "Markdown 문서도 사용 (MyST)" ), this ); myst_->setObjectName( "newSphinx.myst" );
        const auto Row = [ & ]( const QString& Label, QWidget* Field ) { auto* Caption = new QLabel( Label, this ); Caption->setBuddy( Field ); Form->addRow( Caption, Field ); };
        Row( tr( "프로젝트 이름(&N)" ), name_ );
        Row( tr( "저자(&A)" ), author_ );
        { auto* Caption = new QLabel( tr( "위치(&L)" ), this ); Caption->setBuddy( directory_ ); Form->addRow( Caption, Location ); }
        Row( tr( "폴더 이름(&F)" ), folder_ );
        Row( tr( "언어(&G)" ), language_ );
        Row( tr( "테마(&T)" ), theme_ );
        Form->addRow( tr( "구조" ), Structure );
        Form->addRow( QString(), myst_ );
        auto* Left = new QWidget( this ); Left->setLayout( Form ); Left->setMinimumWidth( 380 );
        Columns->addWidget( Left, 0 );

        // 오른쪽: 만들어질 파일과 그 내용.
        preview_ = new QTabWidget( this ); preview_->setObjectName( "newSphinx.preview" ); preview_->setDocumentMode( true );
        tree_ = new QTreeWidget( preview_ ); tree_->setObjectName( "newSphinx.tree" ); tree_->setHeaderHidden( true ); tree_->setColumnCount( 2 );
        tree_->header()->setSectionResizeMode( 0, QHeaderView::ResizeToContents ); tree_->setFrameShape( QFrame::NoFrame ); tree_->setFocusPolicy( Qt::NoFocus );
        conf_ = SourcePreview( preview_, "newSphinx.conf" ); index_ = SourcePreview( preview_, "newSphinx.index" );
        preview_->addTab( tree_, tr( "만들어질 파일" ) ); preview_->addTab( conf_, QStringLiteral( "conf.py" ) ); preview_->addTab( index_, QStringLiteral( "index.rst" ) );
        preview_->setMinimumWidth( 360 );
        Columns->addWidget( preview_, 1 );

        message_ = new QLabel( this ); message_->setObjectName( "newSphinx.message" ); message_->setTextFormat( Qt::PlainText ); message_->setWordWrap( true );
        message_->setMinimumHeight( message_->fontMetrics().lineSpacing() * 2 + 2 ); message_->setAlignment( Qt::AlignLeft | Qt::AlignTop );
        Layout->addWidget( message_ );
        auto* Buttons = new QDialogButtonBox( QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this );
        create_ = Buttons->button( QDialogButtonBox::Ok ); create_->setObjectName( "newSphinx.create" ); create_->setText( tr( "만들기(&O)" ) );
        Buttons->button( QDialogButtonBox::Cancel )->setText( tr( "취소(&C)" ) );
        Layout->addWidget( Buttons );
        connect( Buttons, &QDialogButtonBox::accepted, this, &NewSphinxProjectDialog::accept );
        connect( Buttons, &QDialogButtonBox::rejected, this, &NewSphinxProjectDialog::reject );
        hc::ui::ApplyDialogFrame( this, Buttons, { {}, tr( "이름과 위치를 정하면 conf.py와 index.rst를 만들고 index.rst를 편집 화면으로 엽니다." ), fm::ui::glyph::NewFile } );

        connect( name_, &QLineEdit::textChanged, this, [ this ]( const QString& Text ) {
            // 폴더 이름은 직접 고치기 전까지 프로젝트 이름을 따라간다.
            if( !folderEdited_ ) { const QSignalBlocker Block( folder_ ); folder_->setText( SphinxFolderName( Text ) ); }
            refresh();
        } );
        connect( folder_, &QLineEdit::textEdited, this, [ this ] { folderEdited_ = true; refresh(); } );
        for( auto* Edit : { author_, directory_ } ) connect( Edit, &QLineEdit::textChanged, this, &NewSphinxProjectDialog::refresh );
        for( auto* Combo : { language_, theme_ } ) connect( Combo, &QComboBox::currentIndexChanged, this, &NewSphinxProjectDialog::refresh );
        for( auto* Button : { flat_, separate_ } ) connect( Button, &QRadioButton::toggled, this, &NewSphinxProjectDialog::refresh );
        connect( myst_, &QCheckBox::toggled, this, &NewSphinxProjectDialog::refresh );
        connect( Browse, &QPushButton::clicked, this, [ this ] {
            const auto Chosen = QFileDialog::getExistingDirectory( this, tr( "프로젝트를 둘 위치" ), QDir::fromNativeSeparators( directory_->text() ) );
            if( !Chosen.isEmpty() ) directory_->setText( QDir::toNativeSeparators( Chosen ) );
        } );
        folder_->setText( SphinxFolderName( QString() ) );
        refresh();
        resize( 860, sizeHint().height() );
        name_->setFocus( Qt::OtherFocusReason );
    }

    SphinxProjectSpec NewSphinxProjectDialog::Spec() const
    {
        SphinxProjectSpec Value;
        Value.name_ = name_->text().trimmed();
        Value.author_ = author_->text().trimmed();
        Value.directory_ = QDir::cleanPath( QDir::fromNativeSeparators( directory_->text().trimmed() ) );
        Value.folder_ = folder_->text().trimmed();
        Value.language_ = language_->currentData().toString();
        Value.theme_ = theme_->currentData().toString();
        Value.separate_ = separate_->isChecked();
        Value.myst_ = myst_->isChecked();
        return Value;
    }

    void NewSphinxProjectDialog::refresh()
    {
        if( !message_ || !create_ ) return;
        const auto Value = Spec();
        tree_->clear();
        auto* Root = new QTreeWidgetItem( tree_, { QDir::toNativeSeparators( Value.Root() ) + QDir::separator() } );
        auto* Source = Value.separate_ ? new QTreeWidgetItem( Root, { QStringLiteral( "source" ) + QDir::separator() } ) : Root;
        new QTreeWidgetItem( Source, { QStringLiteral( "conf.py" ), tr( "프로젝트 설정" ) } );
        new QTreeWidgetItem( Source, { QStringLiteral( "index.rst" ), tr( "첫 문서 · 만든 뒤 엽니다" ) } );
        new QTreeWidgetItem( Source, { QStringLiteral( "_static" ) + QDir::separator(), tr( "그림 · CSS" ) } );
        new QTreeWidgetItem( Source, { QStringLiteral( "_templates" ) + QDir::separator() } );
        if( Value.separate_ ) new QTreeWidgetItem( Root, { QStringLiteral( "build" ) + QDir::separator(), tr( "빌드 결과 · 비워 둠" ) } );
        tree_->expandAll();
        conf_->setPlainText( SphinxConfText( Value ) );
        index_->setPlainText( SphinxIndexText( Value ) );

        const auto Error = Value.Error();
        create_->setEnabled( Error.isEmpty() );
        Tint( message_, Error.isEmpty() ? Token::Fg2 : Token::Danger );
        message_->setText( Error.isEmpty() ? tr( "%1에 conf.py와 index.rst를 만들고 index.rst를 엽니다." ).arg( QDir::toNativeSeparators( Value.SourceDirectory() ) ) : Error );
    }

    void NewSphinxProjectDialog::accept()
    {
        if( !create_->isEnabled() ) return;
        const auto Value = Spec();
        QString Index;
        if( const auto Error = CreateSphinxProject( Value, &Index ); !Error.isEmpty() ) {
            Tint( message_, Token::Danger ); message_->setText( Error );
            return;
        }
        QDialog::accept();
        emit Created( Index, Value );
    }

    void NewSphinxProjectDialog::keyPressEvent( QKeyEvent* Event )
    {
        if( !options_.repeatKey_.isEmpty() && QKeySequence( Event->keyCombination() ).matches( options_.repeatKey_ ) == QKeySequence::ExactMatch ) {
            if( !Event->isAutoRepeat() ) accept();
            Event->accept(); return;
        }
        QDialog::keyPressEvent( Event );
    }

    // ── 프로젝트 빌드 ────────────────────────────────────────────────────

    SphinxBuildDialog::SphinxBuildDialog( const QString& ProjectLabel, const QString& ProjectRoot, QWidget* Parent )
        : QDialog( Parent ), projectRoot_( ProjectRoot )
    {
        setObjectName( "sphinxBuildDialog" );
        setWindowTitle( ProjectLabel.isEmpty() ? tr( "빌드" ) : tr( "빌드 — %1" ).arg( ProjectLabel ) );
        const hc::rst::Settings Stored;
        auto* Form = new QFormLayout( this ); Form->setFieldGrowthPolicy( QFormLayout::AllNonFixedFieldsGrow );
        builder_ = new QComboBox( this ); builder_->setObjectName( "sphinxBuild.builder" ); builder_->setEditable( true );
        builder_->addItems( hc::rst::sphinxBuilderPresets() );
        builder_->setCurrentText( Stored.value( QStringLiteral( "build/lastBuilder" ), QStringLiteral( "html" ) ).toString() );
        { auto* Caption = new QLabel( tr( "빌더(&B)" ), this ); Caption->setBuddy( builder_ ); Form->addRow( Caption, builder_ ); }
        output_ = new QLineEdit( this ); output_->setObjectName( "sphinxBuild.output" ); output_->setMinimumWidth( 360 );
        auto* Browse = new QPushButton( tr( "찾아보기…" ), this );
        auto* Row = new QHBoxLayout; Row->setSpacing( 6 ); Row->addWidget( output_, 1 ); Row->addWidget( Browse );
        { auto* Caption = new QLabel( tr( "출력 위치(&O)" ), this ); Caption->setBuddy( output_ ); Form->addRow( Caption, Row ); }
        open_ = new QCheckBox( tr( "빌드가 끝나면 그 폴더 열기" ), this ); open_->setObjectName( "sphinxBuild.open" );
        open_->setChecked( Stored.value( QStringLiteral( "build/openWhenDone" ), true ).toBool() );
        Form->addRow( QString(), open_ );
        auto* Hint = new QLabel( tr( "목록에 없는 빌더는 직접 입력할 수 있습니다. 출력 위치가 없으면 만들어집니다. latexpdf · info는 make 목표라 산출물이 "
                                     "출력 위치 아래의 하위 폴더에 놓입니다. PDF는 LaTeX 도구가 설치돼 있어야 합니다." ), this );
        Hint->setWordWrap( true ); Tint( Hint, Token::Fg2 );
        Form->addRow( Hint );
        auto* Buttons = new QDialogButtonBox( QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this );
        build_ = Buttons->button( QDialogButtonBox::Ok ); build_->setObjectName( "sphinxBuild.build" ); build_->setText( tr( "빌드" ) );
        Buttons->button( QDialogButtonBox::Cancel )->setText( tr( "취소" ) );
        Form->addRow( Buttons );
        connect( Buttons, &QDialogButtonBox::accepted, this, &SphinxBuildDialog::accept );
        connect( Buttons, &QDialogButtonBox::rejected, this, &SphinxBuildDialog::reject );
        hc::ui::ApplyDialogFrame( this, Buttons, { {}, tr( "프로젝트의 conf.py로 Sphinx를 돌립니다. 출력은 빌드 로그에 나오고, 진행 중에는 상태 표시줄에서 취소할 수 있습니다." ), fm::ui::glyph::Refresh } );

        output_->setText( QDir::toNativeSeparators( defaultOutput() ) );
        connect( output_, &QLineEdit::textEdited, this, [ this ] { outputEdited_ = true; refresh(); } );
        connect( builder_, &QComboBox::currentTextChanged, this, [ this ] {
            // 직접 고치기 전까지 출력 위치는 빌더를 따라간다.
            if( !outputEdited_ ) output_->setText( QDir::toNativeSeparators( defaultOutput() ) );
            refresh();
        } );
        connect( Browse, &QPushButton::clicked, this, [ this ] {
            const auto Chosen = QFileDialog::getExistingDirectory( this, tr( "빌드 결과를 놓을 폴더" ), QDir::fromNativeSeparators( output_->text() ) );
            if( !Chosen.isEmpty() ) { outputEdited_ = true; output_->setText( QDir::toNativeSeparators( Chosen ) ); refresh(); }
        } );
        refresh();
        resize( 560, sizeHint().height() );
    }

    QString SphinxBuildDialog::Builder() const { return builder_->currentText().trimmed(); }
    QString SphinxBuildDialog::OutputDirectory() const { return QDir::cleanPath( QDir::fromNativeSeparators( output_->text().trimmed() ) ); }
    QString SphinxBuildDialog::defaultOutput() const { return hc::rst::defaultSphinxOutputDirectory( projectRoot_, Builder() ); }

    void SphinxBuildDialog::refresh()
    {
        // 빌더 이름은 글자 · 숫자 · '-' · '_' 만 받는다( '-' 로 시작하는 옵션 주입을 막는다 ).
        build_->setEnabled( hc::rst::isValidSphinxBuilderName( Builder() ) && !output_->text().trimmed().isEmpty() );
    }

    void SphinxBuildDialog::accept()
    {
        if( !build_->isEnabled() ) return;
        hc::rst::Settings Stored;
        Stored.setValue( QStringLiteral( "build/lastBuilder" ), Builder() );
        Stored.setValue( QStringLiteral( "build/openWhenDone" ), open_->isChecked() );
        QDialog::accept();
    }
}
