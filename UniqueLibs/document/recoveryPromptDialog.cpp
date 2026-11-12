#include "stdafx.h"
#include "recoveryPromptDialog.hpp"

/// 모듈
#include "UniqueLibs/qtDialogFrame.hpp"

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리
#include <fmwidgets/Glyph.h>
#include <fmwidgets/Tag.h>

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QCheckBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

DECLARE_CMNLIBSV2_NAMESPACE

namespace hc::document
{
    namespace
    {
        QString Time( const QDateTime& Value ) { return Value.isValid() ? QLocale().toString( Value.toLocalTime(), QStringLiteral( "yyyy-MM-dd HH:mm" ) ) : QString{}; }
        QString Clock( const QDateTime& Value ) { return Value.isValid() ? QLocale().toString( Value.toLocalTime(), QStringLiteral( "HH:mm" ) ) : QString{}; }
    }

    RecoveryPromptDialog::RecoveryPromptDialog( std::vector< recovery::PromptGroup > Groups, int RetentionDays, QWidget* Parent )
        : QDialog( Parent ), groups_( std::move( Groups ) )
    {
        setObjectName( QStringLiteral( "recoveryPrompt" ) );
        setWindowTitle( tr( "편집 내용 복구" ) );
        auto* Layout = new QVBoxLayout( this ); Layout->setSpacing( 10 );

        auto* Scroll = new QScrollArea( this ); Scroll->setWidgetResizable( true ); Scroll->setFrameShape( QFrame::NoFrame );
        auto* List = new QWidget( Scroll ); auto* Rows = new QVBoxLayout( List ); Rows->setContentsMargins( 0, 0, 0, 0 ); Rows->setSpacing( 10 );
        QDateTime Newest; int Clean = 0, Window = 0;
        for( const auto& Group : groups_ ) {
            if( Group.entries_.empty() ) continue;
            if( !Newest.isValid() || Group.saved_ > Newest ) Newest = Group.saved_;
            Clean += Group.clean_;
            auto* Title = new QLabel( Group.sessionId_.isEmpty() ? tr( "압축 파일 편집본 · %1 저장" ).arg( Clock( Group.saved_ ) )
                : tr( "창 %1 · %2 보존" ).arg( ++Window ).arg( Clock( Group.saved_ ) ), List );
            Title->setForegroundRole( QPalette::PlaceholderText ); Rows->addWidget( Title );
            for( const auto& Entry : Group.entries_ ) {
                auto* Row = new QFrame( List ); Row->setObjectName( QStringLiteral( "recoveryPrompt.row" ) ); Row->setFrameShape( QFrame::StyledPanel );
                auto* Line = new QHBoxLayout( Row ); Line->setContentsMargins( 8, 5, 8, 5 ); Line->setSpacing( 10 );
                auto* Check = new QCheckBox( Row ); Check->setChecked( true ); Check->setAccessibleName( tr( "%1 선택" ).arg( Entry.name_ ) );
                Check->setObjectName( QStringLiteral( "recoveryPrompt.check/" ) + Entry.key_ );
                auto* Names = new QVBoxLayout; Names->setSpacing( 0 );
                auto* Name = new QLabel( Entry.name_, Row ); Name->setTextFormat( Qt::PlainText ); Name->setBuddy( Check );
                auto Font = Name->font(); Font.setWeight( QFont::Medium ); Name->setFont( Font );
                auto* Where = new QLabel( Entry.location_, Row ); Where->setTextFormat( Qt::PlainText ); Where->setForegroundRole( QPalette::PlaceholderText );
                Names->addWidget( Name ); Names->addWidget( Where );
                auto* Chip = new fm::ui::Tag( Row ); Chip->setCompact( true );
                switch( Entry.type_ ) {
                case recovery::PromptEntry::Edit: Chip->setText( tr( "저장하지 않은 변경" ) ); Chip->setTone( fm::ui::Tag::Info ); break;
                case recovery::PromptEntry::Untitled: Chip->setText( tr( "새 문서" ) ); Chip->setTone( fm::ui::Tag::Info ); break;
                case recovery::PromptEntry::Epub: Chip->setText( tr( "EPUB 편집 초안" ) ); Chip->setTone( fm::ui::Tag::Mute ); break;
                case recovery::PromptEntry::Archive: Chip->setText( tr( "압축에 반영 안 됨" ) ); Chip->setTone( fm::ui::Tag::Danger ); break;
                }
                Line->addWidget( Check ); Line->addLayout( Names, 1 ); Line->addWidget( Chip );
                Rows->addWidget( Row ); rows_.append( { Entry.key_, Check } );
                connect( Check, &QCheckBox::toggled, this, &RecoveryPromptDialog::refresh );
            }
        }
        Rows->addStretch( 1 );
        Scroll->setWidget( List ); Layout->addWidget( Scroll, 1 );

        QStringList Hints;
        if( Clean ) Hints << tr( "수정하지 않은 문서 %1개는 복구할 때 함께 다시 엽니다." ).arg( Clean );
        Hints << ( RetentionDays > 0 ? tr( "목록에 보관하면 다시 묻지 않으며, 복구 목록에서 %1일 동안 열 수 있습니다." ).arg( RetentionDays )
                                     : tr( "목록에 보관하면 다시 묻지 않으며, 복구 목록에서 언제든 열 수 있습니다." ) );
        Hints << tr( "선택하지 않은 문서는 목록에 보관합니다." );
        auto* Hint = new QLabel( Hints.join( QChar( ' ' ) ), this ); Hint->setWordWrap( true ); Hint->setForegroundRole( QPalette::PlaceholderText );
        Hint->setObjectName( QStringLiteral( "recoveryPrompt.hint" ) ); Layout->addWidget( Hint );

        auto* Footer = hc::ui::ApplyDialogFrame( this, { tr( "HyperEditor가 정상적으로 종료되지 않았습니다" ),
            tr( "%1에 마지막으로 보존한 내용입니다. 원본 파일은 바뀌지 않았습니다." ).arg( Time( Newest ) ), fm::ui::glyph::Warning } );
        // The frame leaves a stretch first; this row replaces it with its own left and right parts.
        if( auto* Stretch = Footer->takeAt( 0 ) ) delete Stretch;
        buttons_ = new QWidget( Footer->parentWidget() ); auto* Normal = new QHBoxLayout( buttons_ ); Normal->setContentsMargins( 0, 0, 0, 0 ); Normal->setSpacing( 8 );
        delete_ = new QPushButton( tr( "삭제…" ), buttons_ ); delete_->setObjectName( QStringLiteral( "recoveryPrompt.delete" ) );
        count_ = new QLabel( buttons_ ); count_->setObjectName( QStringLiteral( "recoveryPrompt.count" ) ); count_->setForegroundRole( QPalette::PlaceholderText );
        auto* KeepButton = new QPushButton( tr( "목록에 보관" ), buttons_ ); KeepButton->setObjectName( QStringLiteral( "recoveryPrompt.keep" ) );
        restore_ = new QPushButton( tr( "복구" ), buttons_ ); restore_->setObjectName( QStringLiteral( "recoveryPrompt.restore" ) );
        fm::style::setButtonRole( restore_, fm::style::ButtonRole::Primary ); restore_->setDefault( true );
        Normal->addWidget( delete_ ); Normal->addWidget( count_ ); Normal->addStretch( 1 ); Normal->addWidget( KeepButton ); Normal->addWidget( restore_ );
        confirm_ = new QWidget( Footer->parentWidget() ); auto* Confirm = new QHBoxLayout( confirm_ ); Confirm->setContentsMargins( 0, 0, 0, 0 ); Confirm->setSpacing( 8 );
        confirmText_ = new QLabel( confirm_ ); confirmText_->setWordWrap( true ); confirmText_->setObjectName( QStringLiteral( "recoveryPrompt.confirmText" ) );
        auto* Back = new QPushButton( tr( "돌아가기" ), confirm_ ); Back->setObjectName( QStringLiteral( "recoveryPrompt.back" ) );
        auto* Erase = new QPushButton( tr( "삭제" ), confirm_ ); Erase->setObjectName( QStringLiteral( "recoveryPrompt.confirmDelete" ) );
        fm::style::setButtonRole( Erase, fm::style::ButtonRole::Danger );
        Confirm->addWidget( confirmText_, 1 ); Confirm->addWidget( Back ); Confirm->addWidget( Erase );
        confirm_->hide();
        Footer->addWidget( buttons_, 1 ); Footer->addWidget( confirm_, 1 );

        connect( restore_, &QPushButton::clicked, this, [ this ] { finish( Restore ); } );
        connect( KeepButton, &QPushButton::clicked, this, [ this ] { finish( Keep ); } );
        connect( delete_, &QPushButton::clicked, this, [ this, Back ] { buttons_->hide(); confirm_->show(); Back->setFocus(); } );
        connect( Back, &QPushButton::clicked, this, [ this ] { confirm_->hide(); buttons_->show(); delete_->setFocus(); } );
        connect( Erase, &QPushButton::clicked, this, [ this ] { finish( Delete ); } );
        // Closing the window loses nothing: the same as keeping everything in the list.
        connect( this, &QDialog::rejected, this, [ this ] { choice_ = Keep; } );
        refresh();
        resize( 620, qMin( 640, sizeHint().height() + 40 ) );
    }

    QStringList RecoveryPromptDialog::Selected() const
    {
        QStringList Result;
        for( const auto& [ Key, Check ] : rows_ ) if( Check->isChecked() ) Result.append( Key );
        return Result;
    }

    void RecoveryPromptDialog::refresh()
    {
        const auto Checked = int( Selected().size() );
        count_->setText( tr( "%1개 선택 / %2개" ).arg( Checked ).arg( rows_.size() ) );
        restore_->setText( Checked == rows_.size() ? tr( "복구" ) : tr( "선택한 %1개 복구" ).arg( Checked ) );
        restore_->setEnabled( Checked > 0 ); delete_->setEnabled( Checked > 0 );
        confirmText_->setText( tr( "선택한 문서 %1개의 보존된 내용을 삭제합니다. 되돌릴 수 없습니다." ).arg( Checked ) );
    }

    void RecoveryPromptDialog::finish( Choice Value )
    {
        choice_ = Value;
        done( Value == Restore ? QDialog::Accepted : QDialog::Rejected );
        choice_ = Value;   // rejected() above resets a closed window to Keep; an explicit choice stands
    }
}
