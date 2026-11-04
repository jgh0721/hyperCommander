#include "stdafx.h"
#include "textContextMenu.hpp"

/// 모듈

/// UIs
#include "text/QTextView.h"
#include "text/ScintillaQtDirectBackend.h"

/// 기타 헤더파일
#include "documentShortcutRegistry.hpp"

/// 고유 라이브러리
#include "core/AppSettings.h"

/// 공통 라이브러리

/// 외부 라이브러리
#include <Scintilla.h>

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QAction>
#include <QKeySequence>
#include <QMenu>

DECLARE_CMNLIBSV2_NAMESPACE

namespace hc::document
{
    namespace
    {
        QString Tr( const char* Text ) { return QCoreApplication::translate( "hc::document::TextContextMenu", Text ); }
        // Keys are shown only: the window-level command owns the binding.
        QKeySequence ShortcutOf( const QJsonObject& Bindings, const QString& Id )
        {
            return QKeySequence::fromString( Bindings[ Id ].toString(), QKeySequence::PortableText );
        }
    }
    void MoveCaretForContextMenu( QTextView* Text, const QPoint& Position )
    {
        if ( !Text || !Text->Backend() ) return;
        auto* Backend = static_cast< ScintillaQtDirectBackend* >( Text->Backend() );
        const auto Clicked = Backend->Send( SCI_POSITIONFROMPOINTCLOSE, quintptr( Position.x() ), Position.y() );
        if ( Clicked >= 0 && ( Clicked < Backend->Send( SCI_GETSELECTIONSTART ) || Clicked > Backend->Send( SCI_GETSELECTIONEND ) ) )
            Backend->Send( SCI_GOTOPOS, quintptr( Clicked ) );
    }
    void FillTextContextMenu( QMenu& Menu, QTextView* Text, const TextMenuOptions& Options )
    {
        if ( !Text || !Text->Backend() ) return;
        auto* Backend = static_cast< ScintillaQtDirectBackend* >( Text->Backend() );
        AppSettings Settings;
        const auto Stored = Settings.value( "shortcuts/bindings" ).toJsonObject();
        const auto Bindings = DocumentShortcutRegistry::Bindings( DocumentShortcutRegistry::Validate( Stored ).isEmpty() ? Stored : QJsonObject{} );
        const bool Selection = !Backend->selectedText().isEmpty();
        const bool Writable = !Options.readOnly_ && !Text->isReadOnly();
        if ( !Options.leadingText_.isEmpty() ) {
            Menu.addAction( Options.leadingText_, &Menu, [ Run = Options.leading_ ] { if ( Run ) Run(); } );
            Menu.addSeparator();
        }
        auto* Undo = Menu.addAction( Tr( "실행 취소" ), ShortcutOf( Bindings, "edit.undo" ), &Menu, [ Backend, Run = Options.undo_ ] { if ( Run ) Run(); else Backend->Send( SCI_UNDO ); } );
        Undo->setEnabled( Options.canUndo_ ? Options.canUndo_() : Writable && Backend->Send( SCI_CANUNDO ) != 0 );
        auto* Redo = Menu.addAction( Tr( "다시 실행" ), ShortcutOf( Bindings, "edit.redo" ), &Menu, [ Backend, Run = Options.redo_ ] { if ( Run ) Run(); else Backend->Send( SCI_REDO ); } );
        Redo->setEnabled( Options.canRedo_ ? Options.canRedo_() : Writable && Backend->Send( SCI_CANREDO ) != 0 );
        Menu.addSeparator();
        Menu.addAction( Tr( "잘라내기" ), ShortcutOf( Bindings, "edit.cut" ), &Menu, [ Backend ] { Backend->Send( SCI_CUT ); } )->setEnabled( Writable && Selection );
        Menu.addAction( Tr( "복사" ), ShortcutOf( Bindings, "edit.copy" ), &Menu, [ Backend ] { Backend->Send( SCI_COPY ); } )->setEnabled( Selection );
        Menu.addAction( Tr( "붙여넣기" ), ShortcutOf( Bindings, "edit.paste" ), &Menu, [ Backend ] { Backend->Send( SCI_PASTE ); } )->setEnabled( Writable && Backend->Send( SCI_CANPASTE ) != 0 );
        if ( Options.deleteItem_ )
            Menu.addAction( Tr( "삭제" ), QKeySequence( Qt::Key_Delete ), &Menu, [ Backend ] { Backend->Send( SCI_CLEAR ); } )->setEnabled( Writable && Selection );
        Menu.addAction( Options.selectAllText_.isEmpty() ? Tr( "전체 선택" ) : Options.selectAllText_, ShortcutOf( Bindings, "edit.selectAll" ), &Menu, [ Backend ] { Backend->Send( SCI_SELECTALL ); } );
        Menu.addSeparator();
        if ( Options.findInSelection_ )
            Menu.addAction( Tr( "선택 영역에서 찾기…" ), &Menu, [ Text ] { Text->showFindBar( false ); } )->setEnabled( Selection );
        Menu.addAction( Tr( "찾기…" ), ShortcutOf( Bindings, "text.find" ), &Menu, [ Text ] { Text->showFindBar( false ); } );
        Menu.addAction( Tr( "바꾸기…" ), ShortcutOf( Bindings, "text.replace" ), &Menu, [ Text ] { Text->showFindBar( true ); } )->setEnabled( Writable );
        Menu.addAction( Tr( "줄로 이동…" ), ShortcutOf( Bindings, "text.goToLine" ), &Menu, [ Text ] { if ( auto* Go = Text->findChild< QAction* >( "text.goToLine" ) ) Go->trigger(); } );
        Menu.addSeparator();
        if ( Options.fold_ ) {
            auto* Fold = Menu.addMenu( Tr( "접기" ) );
            Fold->addAction( Tr( "전체 접기" ), ShortcutOf( Bindings, "text.foldAll" ), &Menu, [ Text ] { Text->FoldAll( true ); } );
            Fold->addAction( Tr( "전체 펼치기" ), ShortcutOf( Bindings, "text.unfoldAll" ), &Menu, [ Text ] { Text->FoldAll( false ); } );
            Fold->setEnabled( Text->isCodeFoldingEnabled() );
        }
        auto* Wrap = Menu.addAction( Tr( "자동 줄바꿈" ), &Menu, [ Text ]( bool On ) { Text->setWordWrap( On ); } );
        Wrap->setCheckable( true ); Wrap->setChecked( Text->isWordWrapEnabled() );
    }
}
