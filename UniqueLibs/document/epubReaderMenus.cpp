#include "stdafx.h"
#include "epubReaderMenus.hpp"

/// 모듈
#include "epubProcessView.hpp"
#include "documentShortcutRegistry.hpp"
#include "UniqueLibs/ProductDefaults_Edit.hpp"

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QHash>
#include <QInputDialog>
#include <QKeySequence>
#include <QMenu>
#include <QPointer>

namespace hc::document
{
    namespace
    {
        constexpr auto Context = "hc::document::EpubReaderMenus";
        QString Tr( const char* Text ) { return QCoreApplication::translate( Context, Text ); }
        // The reader's highlight colours, in the order and with the names of its notes panel
        // (tools/epub/render/readingAnnotationPanel.cpp); stored notes keep the value.
        struct HighlightColor { const char* name_; const char* value_; };
        constexpr HighlightColor HighlightColors[] = {
            { QT_TRANSLATE_NOOP( "hc::document::EpubReaderMenus", "노랑" ), "#ffde59" },
            { QT_TRANSLATE_NOOP( "hc::document::EpubReaderMenus", "초록" ), "#63d8ad" },
            { QT_TRANSLATE_NOOP( "hc::document::EpubReaderMenus", "파랑" ), "#78baf5" },
            { QT_TRANSLATE_NOOP( "hc::document::EpubReaderMenus", "분홍" ), "#f38ab2" },
        };
    }

    void ShowEpubReaderContextMenu( EpubProcessView* View, QWidget* Parent, const QJsonObject& Request,
                                    std::function< void( const QString& Locator, int Chapter ) > EditAt )
    {
        if ( !View || QApplication::activePopupWidget() ) return;
        const QPointer< EpubProcessView > Guard( View );
        // Keys are shown only: the reader and the window own the bindings.
        const auto Bindings = DocumentShortcutRegistry::Bindings( View->ShortcutBindings() );
        const auto Label = [ &Bindings ]( const QString& Text, const char* Id ) {
            const auto Key = QKeySequence::fromString( Bindings[ QLatin1String( Id ) ].toString(), QKeySequence::PortableText ).toString( QKeySequence::NativeText );
            return Key.isEmpty() ? Text : Text + QLatin1Char( '\t' ) + Key;
        };
        const auto Reader = [ Guard ]( const QString& Action, const QJsonValue& Value = {} ) { return Guard && Guard->ReaderAction( Action, Value ); };
        const auto Selection = Request[ "selection" ].toString().left( 4096 );
        const bool Selected = !Selection.trimmed().isEmpty();
        // With a selection the locator is its range, so a note attaches to it even if the page moved meanwhile.
        const auto Locator = Request[ "locator" ].toString();
        const auto Note = [ Selected, &Locator ]( const QString& Color, const QString& Text ) {
            QJsonObject Value{ { "color", Color }, { "note", Text.left( 2048 ) } };
            if ( Selected && !Locator.isEmpty() ) Value[ "locator" ] = Locator;
            return Value;
        };
        const auto State = View->ViewState();

        QMenu Menu( Parent ? Parent : View );
        auto* Copy = Menu.addAction( Label( Tr( "복사" ), "edit.copy" ) );
        auto* Search = Menu.addAction( Tr( "선택한 글자로 검색" ) );
        auto* Highlight = Menu.addMenu( Tr( "형광펜" ) );
        QHash< QAction*, QString > Colors;
        for ( const auto& Color : HighlightColors ) Colors.insert( Highlight->addAction( Tr( Color.name_ ) ), QString::fromLatin1( Color.value_ ) );
        auto* Memo = Menu.addAction( Tr( "메모 추가…" ) );
        for ( auto* Action : { Copy, Search, Highlight->menuAction(), Memo } ) Action->setEnabled( Selected );
        Menu.addSeparator();
        auto* Bookmark = Menu.addAction( Label( Tr( "책갈피 추가" ), "epub.bookmark-add" ) );
        Menu.addSeparator();
        auto* Previous = Menu.addAction( Label( Tr( "이전 화면" ), "epub.previous" ) );
        auto* Next = Menu.addAction( Label( Tr( "다음 화면" ), "epub.next" ) );
        Previous->setEnabled( !State[ "atStart" ].toBool() ); Next->setEnabled( !State[ "atEnd" ].toBool() );
        auto* Toc = Menu.addAction( Label( Tr( "목차" ), "epub.focus-toc" ) );
        Menu.addSeparator();
        auto* Settings = Menu.addAction( Label( Tr( "읽기 설정…" ), "epub.focus-settings" ) );
        auto* Edit = Menu.addAction( Tr( "편집 모드에서 이 위치 열기" ) );
        Edit->setEnabled( bool( EditAt ) );

        // The reader runs in another process: its point is relative to this view, which it fills.
        auto* Chosen = Menu.exec( View->mapToGlobal( QPoint( Request[ "x" ].toInt(), Request[ "y" ].toInt() ) ) );
        if ( !Guard ) return;
        // Commands that open a panel or the editor keep the focus they move; the rest return it to the page.
        bool Refocus = true;
        if ( Chosen == Copy ) QApplication::clipboard()->setText( Selection );
        else if ( Chosen == Search ) { Refocus = false; Reader( "focus-search" ); Reader( "search", Selection.simplified().left( 128 ) ); }
        else if ( Colors.contains( Chosen ) ) Reader( "annotation-add", Note( Colors.value( Chosen ), {} ) );
        else if ( Chosen == Memo ) {
            bool Accepted = false;
            const auto Text = QInputDialog::getMultiLineText( Parent ? Parent : View, Tr( "메모 추가" ), Tr( "선택한 문장에 붙일 메모" ), {}, &Accepted );
            if ( !Guard ) return;
            if ( Accepted ) Reader( "annotation-add", Note( QString::fromLatin1( hc::defaults::edit::AnnotationColor ), Text ) );
        }
        else if ( Chosen == Bookmark ) Reader( "bookmark-add" );
        else if ( Chosen == Previous ) Reader( "previous" );
        else if ( Chosen == Next ) Reader( "next" );
        else if ( Chosen == Toc ) { Refocus = false; Reader( "toggle-toc" ); }
        else if ( Chosen == Settings ) { Refocus = false; Reader( "toggle-settings" ); }
        else if ( Chosen == Edit ) { Refocus = false; EditAt( Locator, Request[ "chapter" ].toInt( -1 ) ); }
        if ( Refocus && Guard ) Guard->FocusInput();
    }
}
