#include "stdafx.h"
#include "epubTemplate.hpp"

/// 모듈
#include "epubArchive.hpp"
#include "epubPackage.hpp"

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QDateTime>
#include <QRegularExpression>
#include <QStringList>
#include <QUuid>
#include <memory>
#include <new>

namespace hc::document::epub
{
    namespace
    {
        // Text and attribute values are double-quoted; < > & " are the only characters to escape.
        QString escaped( const QString& Value ) { return Value.toHtmlEscaped(); }

        QString extension( const QString& Media )
        {
            if( Media == "image/jpeg" ) return QStringLiteral( "jpg" );
            return Media.section( '/', 1 );
        }

        QString xhtmlOpen( const NewBook& Book, const QString& Title, const QString& Stylesheet )
        {
            QStringList Lines{
                QStringLiteral( "<?xml version=\"1.0\" encoding=\"utf-8\"?>" ),
                QStringLiteral( "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\" xml:lang=\"%1\" lang=\"%1\">" ).arg( escaped( Book.language_ ) ),
                QStringLiteral( "<head>" ),
                QStringLiteral( "  <meta charset=\"utf-8\"/>" ),
                QStringLiteral( "  <title>%1</title>" ).arg( escaped( Title ) ) };
            if( !Stylesheet.isEmpty() ) Lines.append( QStringLiteral( "  <link rel=\"stylesheet\" type=\"text/css\" href=\"%1\"/>" ).arg( Stylesheet ) );
            Lines.append( QStringLiteral( "</head>" ) );
            return Lines.join( '\n' ) + '\n';
        }

        QByteArray container()
        {
            return QByteArrayLiteral(
                "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
                "  <rootfiles>\n"
                "    <rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/>\n"
                "  </rootfiles>\n"
                "</container>\n" );
        }

        QByteArray package( const NewBook& Book, const QString& CoverMedia )
        {
            const bool Cover = !CoverMedia.isEmpty();
            QStringList Lines{
                QStringLiteral( "<?xml version=\"1.0\" encoding=\"utf-8\"?>" ),
                QStringLiteral( "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"book-id\" xml:lang=\"%1\">" ).arg( escaped( Book.language_ ) ),
                QStringLiteral( "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">" ),
                QStringLiteral( "    <dc:identifier id=\"book-id\">urn:uuid:%1</dc:identifier>" ).arg( QUuid::createUuid().toString( QUuid::WithoutBraces ) ),
                QStringLiteral( "    <dc:title>%1</dc:title>" ).arg( escaped( Book.title_ ) ) };
            if( !Book.creator_.isEmpty() ) Lines.append( QStringLiteral( "    <dc:creator>%1</dc:creator>" ).arg( escaped( Book.creator_ ) ) );
            Lines.append( QStringLiteral( "    <dc:language>%1</dc:language>" ).arg( escaped( Book.language_ ) ) );
            Lines.append( QStringLiteral( "    <meta property=\"dcterms:modified\">%1</meta>" )
                .arg( QDateTime::currentDateTimeUtc().toString( QStringLiteral( "yyyy-MM-dd'T'HH:mm:ss'Z'" ) ) ) );
            if( Cover ) Lines.append( QStringLiteral( "    <meta name=\"cover\" content=\"cover-image\"/>" ) );
            Lines.append( QStringLiteral( "  </metadata>" ) );
            Lines.append( QStringLiteral( "  <manifest>" ) );
            Lines.append( QStringLiteral( "    <item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>" ) );
            if( Cover ) Lines.append( QStringLiteral( "    <item id=\"cover\" href=\"Text/cover.xhtml\" media-type=\"application/xhtml+xml\"/>" ) );
            Lines.append( QStringLiteral( "    <item id=\"chapter-001\" href=\"Text/chapter-001.xhtml\" media-type=\"application/xhtml+xml\"/>" ) );
            if( Book.stylesheet_ ) Lines.append( QStringLiteral( "    <item id=\"book-css\" href=\"Styles/book.css\" media-type=\"text/css\"/>" ) );
            if( Cover ) Lines.append( QStringLiteral( "    <item id=\"cover-image\" href=\"Images/cover.%1\" media-type=\"%2\" properties=\"cover-image\"/>" )
                .arg( extension( CoverMedia ), CoverMedia ) );
            Lines.append( QStringLiteral( "  </manifest>" ) );
            Lines.append( QStringLiteral( "  <spine>" ) );
            if( Cover ) Lines.append( QStringLiteral( "    <itemref idref=\"cover\"/>" ) );
            Lines.append( QStringLiteral( "    <itemref idref=\"chapter-001\"/>" ) );
            Lines.append( QStringLiteral( "  </spine>" ) );
            Lines.append( QStringLiteral( "</package>" ) );
            return ( Lines.join( '\n' ) + '\n' ).toUtf8();
        }

        QByteArray chapter( const NewBook& Book )
        {
            auto Text = xhtmlOpen( Book, Book.chapterTitle_, Book.stylesheet_ ? QStringLiteral( "../Styles/book.css" ) : QString{} );
            Text += QStringLiteral(
                "<body>\n"
                "  <section epub:type=\"chapter\" role=\"doc-chapter\">\n"
                "    <h1>%1</h1>\n"
                "    <p></p>\n"
                "  </section>\n"
                "</body>\n"
                "</html>\n" ).arg( escaped( Book.chapterTitle_ ) );
            return Text.toUtf8();
        }

        QByteArray cover( const NewBook& Book, const QString& CoverMedia )
        {
            auto Text = xhtmlOpen( Book, Book.title_, {} );
            Text += QStringLiteral(
                "<body>\n"
                "  <section epub:type=\"cover\">\n"
                "    <img src=\"../Images/cover.%1\" alt=\"%2\"/>\n"
                "  </section>\n"
                "</body>\n"
                "</html>\n" ).arg( extension( CoverMedia ), escaped( Book.title_ ) );
            return Text.toUtf8();
        }

        QByteArray navigation( const NewBook& Book )
        {
            // The navigation heading follows the book language, not the UI language.
            const auto Heading = Book.language_.startsWith( QStringLiteral( "ko" ), Qt::CaseInsensitive ) ? QStringLiteral( "목차" ) : QStringLiteral( "Contents" );
            auto Text = xhtmlOpen( Book, Heading, {} );
            Text += QStringLiteral(
                "<body>\n"
                "  <nav epub:type=\"toc\" id=\"toc\">\n"
                "    <h1>%1</h1>\n"
                "    <ol>\n"
                "      <li><a href=\"Text/chapter-001.xhtml\">%2</a></li>\n"
                "    </ol>\n"
                "  </nav>\n"
                "</body>\n"
                "</html>\n" ).arg( escaped( Heading ), escaped( Book.chapterTitle_ ) );
            return Text.toUtf8();
        }

        QByteArray stylesheet()
        {
            return QByteArrayLiteral(
                "body {\n"
                "  margin: 0 5%;\n"
                "  line-height: 1.8;\n"
                "  word-break: keep-all;\n"
                "  overflow-wrap: break-word;\n"
                "}\n"
                "h1 {\n"
                "  font-size: 1.5em;\n"
                "  margin: 2em 0 1.5em;\n"
                "  text-align: center;\n"
                "}\n"
                "p {\n"
                "  margin: 0;\n"
                "  text-indent: 1em;\n"
                "  text-align: justify;\n"
                "}\n" );
        }

        EditOutput failure( const QString& Code, const QString& Location = {} )
        {
            EditOutput Result; Result.error_ = Code; Result.location_ = Location; return Result;
        }
    }

    bool ValidLanguageTag( const QString& Value )
    {
        static const QRegularExpression Pattern( QStringLiteral( "^[A-Za-z]{2,8}(?:-[A-Za-z0-9]{1,8})*$" ) );
        return Value.size() <= 128 && Pattern.match( Value ).hasMatch();
    }

    bool ValidBookText( const QString& Value, bool Required )
    {
        if( Required && Value.trimmed().isEmpty() ) return false;
        if( Value.size() > 1024 || QString::fromUtf8( Value.toUtf8() ) != Value ) return false;
        for( const auto Char : Value )
            if( Char.unicode() < 0x20 || Char.unicode() == 0x7f || Char.unicode() == 0xfffe || Char.unicode() == 0xffff ) return false;
        return true;
    }

    EditOutput CreateBook( const NewBook& Input, const Cancellation& Cancel )
    {
        NewBook Book = Input;
        Book.title_ = Book.title_.trimmed(); Book.creator_ = Book.creator_.trimmed();
        Book.language_ = Book.language_.trimmed(); Book.chapterTitle_ = Book.chapterTitle_.trimmed();
        if( !ValidBookText( Book.title_, true ) ) return failure( QStringLiteral( "edit-new-title" ) );
        if( !ValidBookText( Book.creator_, false ) ) return failure( QStringLiteral( "edit-new-creator" ) );
        if( !ValidLanguageTag( Book.language_ ) ) return failure( QStringLiteral( "edit-new-language" ) );
        if( !ValidBookText( Book.chapterTitle_, true ) ) return failure( QStringLiteral( "edit-new-chapter-title" ) );
        QString CoverMedia;
        if( !Book.coverImage_.isEmpty() ) {
            CoverMedia = ImageMediaType( Book.coverImage_ );
            if( CoverMedia.isEmpty() ) return failure( QStringLiteral( "edit-new-cover-image" ) );
        }
        try {
            detail::StoredZipWriter Writer( Cancel );
            Writer.Add( QStringLiteral( "mimetype" ), QByteArrayLiteral( "application/epub+zip" ) );
            Writer.Add( QStringLiteral( "META-INF/container.xml" ), container() );
            Writer.Add( QStringLiteral( "OEBPS/content.opf" ), package( Book, CoverMedia ) );
            Writer.Add( QStringLiteral( "OEBPS/nav.xhtml" ), navigation( Book ) );
            if( !CoverMedia.isEmpty() ) Writer.Add( QStringLiteral( "OEBPS/Text/cover.xhtml" ), cover( Book, CoverMedia ) );
            Writer.Add( QString::fromLatin1( NewBookFirstChapter ), chapter( Book ) );
            if( Book.stylesheet_ ) Writer.Add( QStringLiteral( "OEBPS/Styles/book.css" ), stylesheet() );
            if( !CoverMedia.isEmpty() ) Writer.Add( QStringLiteral( "OEBPS/Images/cover.%1" ).arg( extension( CoverMedia ) ), Book.coverImage_ );
            auto Bytes = Writer.Finish();
            // The same pair Structure() relies on: the editor model and the reader's publication index.
            auto Model = std::make_shared<EditModel>();
            if( !Model->Load( Bytes, Cancel ) ) return failure( QStringLiteral( "edit-new-self-check" ), Model->Error() );
            const auto Session = PublicationSession::Open( Bytes, Cancel );
            const auto Index = Session ? Session->Index() : PackageResult{};
            if( !Index ) return failure( QStringLiteral( "edit-new-self-check" ), Index.error_ + ": " + Index.location_ );
            EditOutput Result; Result.bytes_ = std::move( Bytes ); Result.prepared_ = std::move( Model ); return Result;
        } catch( const detail::Rejected& Error ) {
            return failure( Error.code_, Error.location_ );
        } catch( const std::bad_alloc& ) {
            return failure( QStringLiteral( "edit-new-memory-limit" ) );
        }
    }
}
