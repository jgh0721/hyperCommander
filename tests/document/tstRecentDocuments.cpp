#include "stdafx.h"
#include "UniqueLibs/document/recentDocuments.hpp"
#include "UniqueLibs/document/recentDocumentsDialog.hpp"
#include "UniqueLibs/document/documentStartPage.hpp"
#include <QAbstractButton>
#include <QApplication>
#include <QDropEvent>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QListWidget>
#include <QLockFile>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>
#include <QtTest>
#include <algorithm>
#include <memory>

Q_DECLARE_METATYPE( hc::document::RecentRecordContext )

using namespace hc::document;

class RecentDocumentsTests : public QObject
{
    Q_OBJECT

    static RecentDocument Doc( const QString& Path, const QString& Kind = QStringLiteral( "text" ), const QDateTime& At = {} )
    {
        RecentDocument Document;
        Document.path_     = Path;
        Document.kind_     = Kind;
        Document.viewId_   = Kind;
        Document.openedAt_ = At;
        return Document;
    }
    static QByteArray Read( const QString& Path )
    {
        QFile File( Path );
        return File.open( QIODevice::ReadOnly ) ? File.readAll() : QByteArray();
    }
    static bool Write( const QString& Path, const QByteArray& Bytes )
    {
        QFile File( Path );
        return File.open( QIODevice::WriteOnly | QIODevice::Truncate ) && File.write( Bytes ) == Bytes.size();
    }
    // 순서와 무관하게 비교하기 위한 요약: "키|고정".
    static QStringList Summary( const QList< RecentDocument >& Entries )
    {
        QStringList Result;
        for( const auto& Entry : Entries ) Result.append( Entry.Key() + ( Entry.pinned_ ? QStringLiteral( "|pinned" ) : QString() ) );
        Result.sort();
        return Result;
    }
    static bool Contains( const QList< RecentDocument >& Entries, const QString& Path )
    {
        const auto Key = RecentKey( Path );
        return std::any_of( Entries.cbegin(), Entries.cend(), [ &Key ]( const RecentDocument& Entry ) { return Entry.Key() == Key; } );
    }
    static QList< QAbstractButton* > VisibleRows( QWidget* Page )
    {
        QList< QAbstractButton* > Rows;
        for( auto* Row : Page->findChildren< QAbstractButton* >( QStringLiteral( "startRecentRow" ) ) )
            if( !Row->isHidden() ) Rows.append( Row );
        return Rows;
    }
    static int VisibleItems( QListWidget* List )
    {
        int Count = 0;
        for( int Index = 0; Index < List->count(); ++Index ) Count += List->item( Index )->isHidden() ? 0 : 1;
        return Count;
    }

private slots:
    void initTestCase()
    {
        qRegisterMetaType< hc::document::RecentDocument >();
    }

    void keyNormalisation()
    {
        const auto Expected = RecentKey( QStringLiteral( "D:/Docs/Sample.TXT" ) );
        QCOMPARE( Expected, QStringLiteral( "d:/docs/sample.txt" ) );
        QCOMPARE( RecentKey( QStringLiteral( "D:\\Docs\\Sample.txt" ) ), Expected );
        QCOMPARE( RecentKey( QStringLiteral( "d:\\DOCS\\sub\\..\\sample.txt" ) ), Expected );
        QCOMPARE( RecentKey( QStringLiteral( "D:/Docs//./Sample.txt" ) ), Expected );
        QCOMPARE( Doc( QStringLiteral( "D:\\Docs\\Other\\..\\SAMPLE.txt" ) ).Key(), Expected );
        QVERIFY( RecentKey( QStringLiteral( "D:/Docs/Sample2.txt" ) ) != Expected );
        QVERIFY( RecentKey( {} ).isEmpty() );
    }

    void shouldRecord_data()
    {
        QTest::addColumn< RecentRecordContext >( "context" );
        QTest::addColumn< bool >( "expected" );
        const auto With = []( auto Change ) { RecentRecordContext Context; Change( Context ); return Context; };
        QTest::newRow( "plain" ) << RecentRecordContext() << true;
        QTest::newRow( "disabled" ) << With( []( auto& C ) { C.enabled_ = false; } ) << false;
        QTest::newRow( "capture copy" ) << With( []( auto& C ) { C.temporarySource_ = true; } ) << false;
        QTest::newRow( "document copy" ) << With( []( auto& C ) { C.originalAuthority_ = false; } ) << false;
        QTest::newRow( "recovered" ) << With( []( auto& C ) { C.recovered_ = true; } ) << false;
        QTest::newRow( "untitled" ) << With( []( auto& C ) { C.untitled_ = true; } ) << false;
        QTest::newRow( "failed" ) << With( []( auto& C ) { C.failed_ = true; } ) << false;
        QTest::newRow( "private reading" ) << With( []( auto& C ) { C.privateReading_ = true; } ) << false;
        QTest::newRow( "broker copy" ) << With( []( auto& C ) { C.brokerCopy_ = true; } ) << false;
    }
    void shouldRecord()
    {
        QFETCH( RecentRecordContext, context );
        QFETCH( bool, expected );
        QCOMPARE( ShouldRecordRecent( context ), expected );
    }

    void description_data()
    {
        QTest::addColumn< RecentDocument >( "document" );
        QTest::addColumn< QString >( "expected" );
        const QString Path = QStringLiteral( "D:\\Docs\\샘플\\sample.bin" );
        const auto    With = [ &Path ]( const QString& Kind, auto Change ) { auto Document = Doc( Path, Kind ); Change( Document ); return Document; };
        const auto    Same = []( auto& ) {};
        QTest::newRow( "text" ) << With( "text", Same ) << QStringLiteral( "텍스트 · …\\샘플" );
        QTest::newRow( "epub edit" ) << With( "epub", []( auto& D ) { D.epubMode_ = "edit"; } ) << QStringLiteral( "EPUB 편집 · …\\샘플" );
        QTest::newRow( "epub read" ) << With( "epub", []( auto& D ) { D.epubMode_ = "read"; } ) << QStringLiteral( "EPUB 읽기 · …\\샘플" );
        QTest::newRow( "comic resume" ) << With( "comic", []( auto& D ) { D.readingPage_ = 5; D.readingPageCount_ = 12; } ) << QStringLiteral( "만화 · 5 / 12쪽에서 이어 읽기" );
        QTest::newRow( "comic fresh" ) << With( "comic", Same ) << QStringLiteral( "만화 · …\\샘플" );
        QTest::newRow( "webtoon resume" ) << With( "webtoon", []( auto& D ) { D.readingPage_ = 5; D.readingPageCount_ = 12; } ) << QStringLiteral( "웹툰 · 이미지 5 / 12에서 이어 읽기" );
        QTest::newRow( "pdf page" ) << With( "pdf", []( auto& D ) { D.pdfPage_ = 3; } ) << QStringLiteral( "PDF · 3쪽" );
        QTest::newRow( "markdown split" ) << With( "markdown", []( auto& D ) { D.markdownView_ = "split"; } ) << QStringLiteral( "Markdown · 나란히 보기" );
        QTest::newRow( "markdown preview" ) << With( "markdown", []( auto& D ) { D.markdownView_ = "preview"; } ) << QStringLiteral( "Markdown · 미리보기" );
        QTest::newRow( "markdown source" ) << With( "markdown", []( auto& D ) { D.markdownView_ = "source"; } ) << QStringLiteral( "Markdown · 소스 편집" );
        QTest::newRow( "image" ) << With( "image", Same ) << QStringLiteral( "이미지 · …\\샘플" );
        QTest::newRow( "hwp" ) << With( "hwp", Same ) << QStringLiteral( "한글 문서 · …\\샘플" );
        QTest::newRow( "drive root" ) << Doc( QStringLiteral( "D:\\sample.txt" ) ) << QStringLiteral( "텍스트 · D:\\" );
    }
    void description()
    {
        QFETCH( RecentDocument, document );
        QFETCH( QString, expected );
        QCOMPARE( RecentDescription( document ), expected );
    }

    void dateText_data()
    {
        QTest::addColumn< QDateTime >( "when" );
        QTest::addColumn< QDateTime >( "now" );
        QTest::addColumn< QString >( "expected" );
        const QDateTime Now( QDate( 2026, 11, 4 ), QTime( 18, 0 ) );
        QTest::newRow( "today" ) << QDateTime( QDate( 2026, 11, 4 ), QTime( 14, 20 ) ) << Now << QStringLiteral( "오늘 14:20" );
        QTest::newRow( "yesterday" ) << QDateTime( QDate( 2026, 11, 3 ), QTime( 23, 59 ) ) << Now << QStringLiteral( "어제" );
        QTest::newRow( "this year" ) << QDateTime( QDate( 2026, 11, 2 ), QTime( 9, 0 ) ) << Now << QStringLiteral( "11월 2일" );
        QTest::newRow( "other year" ) << QDateTime( QDate( 2025, 11, 2 ), QTime( 9, 0 ) ) << Now << QStringLiteral( "2025년 11월 2일" );
        QTest::newRow( "yesterday across year" ) << QDateTime( QDate( 2025, 12, 31 ), QTime( 23, 0 ) ) << QDateTime( QDate( 2026, 1, 1 ), QTime( 0, 30 ) ) << QStringLiteral( "어제" );
    }
    void dateText()
    {
        QFETCH( QDateTime, when );
        QFETCH( QDateTime, now );
        QFETCH( QString, expected );
        QCOMPARE( RecentDateText( when, now ), expected );
    }

    void jsonFormat()
    {
        QTemporaryDir Dir;
        QVERIFY( Dir.isValid() );
        // 빠진 필드 · 모르는 필드 · 경로 없는 항목을 견딘다.
        QVERIFY( Write( QDir( Dir.path() ).filePath( "recent-documents.json" ),
                        R"({"schemaVersion":1,"entries":[{"path":"D:\\Docs\\a.md","kind":"markdown","markdownView":"split","future":42},{"kind":"text"},{"path":""}]})" ) );
        RecentDocumentStore Store( Dir.path() );
        QVERIFY( Store.LoadSync() );
        QCOMPARE( Store.Entries().size(), 1 );
        QCOMPARE( Store.Entries().first().markdownView_, QStringLiteral( "split" ) );
        QCOMPARE( Store.Entries().first().pdfPage_, 0 );

        auto Pdf     = Doc( Dir.filePath( "b.pdf" ), "pdf", QDateTime::currentDateTime() );
        Pdf.pdfPage_ = 3;
        Store.Record( Pdf );
        QVERIFY( Store.WaitForIdle( 5000 ) );
        const auto Root = QJsonDocument::fromJson( Read( Store.FileName() ) ).object();
        QCOMPARE( Root.value( "schemaVersion" ).toInt(), 1 );
        const auto Entries = Root.value( "entries" ).toArray();
        QCOMPARE( Entries.size(), 2 );
        const auto First = Entries.first().toObject();
        QCOMPARE( RecentKey( First.value( "path" ).toString() ), RecentKey( Pdf.path_ ) );
        QCOMPARE( First.value( "pdfPage" ).toInt(), 3 );
        QVERIFY( !First.contains( "pinned" ) );
        QVERIFY( !First.contains( "encoding" ) );
        QVERIFY( !First.contains( "readingPage" ) );
        QVERIFY( QRegularExpression( "^\\d{4}-\\d\\d-\\d\\dT\\d\\d:\\d\\d:\\d\\d(\\.\\d+)?(Z|[+-]\\d\\d:\\d\\d)$" ).match( First.value( "openedAt" ).toString() ).hasMatch() );
        QVERIFY( !Entries.at( 1 ).toObject().contains( "future" ) );
    }

    void trimLimits()
    {
        QTemporaryDir Dir;
        QVERIFY( Dir.isValid() );
        RecentDocumentStore Store( Dir.path() );
        QVERIFY( Store.LoadSync() );
        const auto Base = QDateTime::currentDateTime().addSecs( -3600 );
        for( int Index = 0; Index < 35; ++Index ) Store.Record( Doc( Dir.filePath( QString( "f%1.txt" ).arg( Index ) ), "text", Base.addSecs( Index ) ) );
        QCOMPARE( Store.Entries().size(), RecentDocumentStore::KeepUnpinned );
        QVERIFY( !Contains( Store.Entries(), Dir.filePath( "f4.txt" ) ) );
        QVERIFY( Contains( Store.Entries(), Dir.filePath( "f5.txt" ) ) );
        QCOMPARE( RecentKey( Store.Entries().first().path_ ), RecentKey( Dir.filePath( "f34.txt" ) ) );

        // 고정 한도를 넘으면 가장 오래전에 고정한 항목부터 고정이 풀린다.
        for( int Index = 5; Index < 17; ++Index ) {
            Store.Pin( RecentKey( Dir.filePath( QString( "f%1.txt" ).arg( Index ) ) ), true );
            QThread::msleep( 3 );
        }
        QVERIFY( Store.WaitForIdle( 5000 ) );
        const auto Entries = Store.Entries();
        const auto Pinned  = std::count_if( Entries.cbegin(), Entries.cend(), []( const RecentDocument& Entry ) { return Entry.pinned_; } );
        QCOMPARE( int( Pinned ), RecentDocumentStore::KeepPinned );
        for( int Index = 0; Index < RecentDocumentStore::KeepPinned; ++Index ) QVERIFY( Entries.at( Index ).pinned_ );
        const auto Last = std::find_if( Entries.cbegin(), Entries.cend(), [ &Dir ]( const RecentDocument& Entry ) { return Entry.Key() == RecentKey( Dir.filePath( "f16.txt" ) ); } );
        QVERIFY( Last != Entries.cend() && Last->pinned_ );
        const auto First = std::find_if( Entries.cbegin(), Entries.cend(), [ &Dir ]( const RecentDocument& Entry ) { return Entry.Key() == RecentKey( Dir.filePath( "f5.txt" ) ); } );
        QVERIFY( First != Entries.cend() && !First->pinned_ );

        RecentDocumentStore Reader( Dir.path() );
        QVERIFY( Reader.LoadSync() );
        QCOMPARE( Summary( Reader.Entries() ), Summary( Entries ) );
    }

    void concurrentStoresConverge()
    {
        QTemporaryDir Dir;
        QVERIFY( Dir.isValid() );
        RecentDocumentStore A( Dir.path() ), B( Dir.path() );
        QVERIFY( A.LoadSync() );
        QVERIFY( B.LoadSync() );
        const auto Base = QDateTime::currentDateTime().addSecs( -3600 );
        for( int Index = 0; Index < 10; ++Index ) {
            A.Record( Doc( Dir.filePath( QString( "a%1.txt" ).arg( Index ) ), "text", Base.addSecs( 2 * Index ) ) );
            B.Record( Doc( Dir.filePath( QString( "b%1.txt" ).arg( Index ) ), "text", Base.addSecs( 2 * Index + 1 ) ) );
        }
        QVERIFY( A.WaitForIdle( 10000 ) );
        QVERIFY( B.WaitForIdle( 10000 ) );
        {
            RecentDocumentStore Reader( Dir.path() );
            QVERIFY( Reader.LoadSync() );
            QCOMPARE( Reader.Entries().size(), 20 );
        }
        A.ReloadIfChanged();
        B.ReloadIfChanged();
        QVERIFY( A.WaitForIdle( 5000 ) );
        QVERIFY( B.WaitForIdle( 5000 ) );
        QCOMPARE( A.Entries().size(), 20 );
        QCOMPARE( Summary( A.Entries() ), Summary( B.Entries() ) );

        // B 는 a0 을 아직 메모리에 가진 채로 쓴다. A 가 지운 a0 이 되살아나면 안 된다.
        A.Remove( RecentKey( Dir.filePath( "a0.txt" ) ) );
        B.Pin( RecentKey( Dir.filePath( "a1.txt" ) ), true );
        B.Record( Doc( Dir.filePath( "b10.txt" ) ) );
        A.Record( Doc( Dir.filePath( "a10.txt" ) ) );
        B.Remove( RecentKey( Dir.filePath( "b0.txt" ) ) );
        QVERIFY( A.WaitForIdle( 10000 ) );
        QVERIFY( B.WaitForIdle( 10000 ) );

        RecentDocumentStore Reader( Dir.path() );
        QVERIFY( Reader.LoadSync() );
        const auto Disk = Reader.Entries();
        QCOMPARE( Disk.size(), 20 );
        QVERIFY( !Contains( Disk, Dir.filePath( "a0.txt" ) ) );
        QVERIFY( !Contains( Disk, Dir.filePath( "b0.txt" ) ) );
        QVERIFY( Contains( Disk, Dir.filePath( "a10.txt" ) ) );
        QVERIFY( Contains( Disk, Dir.filePath( "b10.txt" ) ) );
        QVERIFY( Disk.first().pinned_ );
        QCOMPARE( Disk.first().Key(), RecentKey( Dir.filePath( "a1.txt" ) ) );

        A.ReloadIfChanged();
        B.ReloadIfChanged();
        QVERIFY( A.WaitForIdle( 5000 ) );
        QVERIFY( B.WaitForIdle( 5000 ) );
        QCOMPARE( Summary( A.Entries() ), Summary( Disk ) );
        QCOMPARE( Summary( B.Entries() ), Summary( Disk ) );
    }

    void lockRetryThenSuccess()
    {
        QTemporaryDir Dir;
        QVERIFY( Dir.isValid() );
        RecentDocumentStore Store( Dir.path() );
        QVERIFY( Store.LoadSync() );
        QLockFile Foreign( Store.FileName() + ".lock" );
        QVERIFY( Foreign.tryLock( 0 ) );
        QSignalSpy Changed( &Store, &RecentDocumentStore::Changed );
        Store.Record( Doc( Dir.filePath( "locked.txt" ) ) );
        // 메모리 목록은 바로 바뀌고, 디스크 쓰기는 잠금이 풀릴 때까지 미룬다.
        QCOMPARE( Changed.count(), 1 );
        QCOMPARE( Store.Entries().size(), 1 );
        QTest::qWait( 900 );
        QVERIFY( !QFileInfo::exists( Store.FileName() ) );
        QVERIFY( !Store.WaitForIdle( 0 ) );
        Foreign.unlock();
        QVERIFY( Store.WaitForIdle( 5000 ) );
        RecentDocumentStore Reader( Dir.path() );
        QVERIFY( Reader.LoadSync() );
        QVERIFY( Contains( Reader.Entries(), Dir.filePath( "locked.txt" ) ) );
    }

    void lockFailuresParkUntilNextChange()
    {
        QTemporaryDir Dir;
        QVERIFY( Dir.isValid() );
        RecentDocumentStore Store( Dir.path() );
        QVERIFY( Store.LoadSync() );
        QLockFile Foreign( Store.FileName() + ".lock" );
        QVERIFY( Foreign.tryLock( 0 ) );
        Store.Record( Doc( Dir.filePath( "first.txt" ) ) );
        // 세 번(약 0.5 + 1 + 0.5 + 1 + 0.5 초) 실패하면 다시 시도하지 않고 다음 변경을 기다린다.
        QTest::qWait( 5500 );
        Foreign.unlock();
        QTest::qWait( 1500 );
        QVERIFY( !QFileInfo::exists( Store.FileName() ) );
        Store.Record( Doc( Dir.filePath( "second.txt" ) ) );
        QVERIFY( Store.WaitForIdle( 5000 ) );
        RecentDocumentStore Reader( Dir.path() );
        QVERIFY( Reader.LoadSync() );
        QVERIFY( Contains( Reader.Entries(), Dir.filePath( "first.txt" ) ) );
        QVERIFY( Contains( Reader.Entries(), Dir.filePath( "second.txt" ) ) );
    }

    void corruptJsonIsPreserved()
    {
        QTemporaryDir Dir;
        QVERIFY( Dir.isValid() );
        RecentDocumentStore Store( Dir.path() );
        const QByteArray First = "{ \"schemaVersion\": 1, \"entries\": [ broken";
        QVERIFY( Write( Store.FileName(), First ) );
        QVERIFY( Store.LoadSync() );
        QVERIFY( Store.Entries().isEmpty() );
        QCOMPARE( Read( Store.FileName() + ".bad" ), First );
        QVERIFY( !QFileInfo::exists( Store.FileName() ) );

        Store.Record( Doc( Dir.filePath( "one.txt" ) ) );
        QVERIFY( Store.WaitForIdle( 5000 ) );
        QVERIFY( QJsonDocument::fromJson( Read( Store.FileName() ) ).isObject() );

        // 쓰기 때 손상을 만나도 .bad 를 새 내용으로 바꾸고 빈 목록에서 시작한다.
        const QByteArray Second = "not json at all";
        QVERIFY( Write( Store.FileName(), Second ) );
        Store.Record( Doc( Dir.filePath( "two.txt" ) ) );
        QVERIFY( Store.WaitForIdle( 5000 ) );
        QCOMPARE( Read( Store.FileName() + ".bad" ), Second );
        RecentDocumentStore Reader( Dir.path() );
        QVERIFY( Reader.LoadSync() );
        QCOMPARE( Reader.Entries().size(), 1 );
        QVERIFY( Contains( Reader.Entries(), Dir.filePath( "two.txt" ) ) );
        QCOMPARE( Summary( Store.Entries() ), Summary( Reader.Entries() ) );
    }

    void asyncLoadEmitsChanged()
    {
        QTemporaryDir Dir;
        QVERIFY( Dir.isValid() );
        {
            RecentDocumentStore Writer( Dir.path() );
            Writer.Record( Doc( Dir.filePath( "kept.txt" ) ) );
        }   // 소멸자가 남은 쓰기를 마친다.
        RecentDocumentStore Store( Dir.path() );
        QSignalSpy Changed( &Store, &RecentDocumentStore::Changed );
        Store.Load();
        QVERIFY( Store.WaitForIdle( 5000 ) );
        QCOMPARE( Changed.count(), 1 );
        QVERIFY( Contains( Store.Entries(), Dir.filePath( "kept.txt" ) ) );
        Store.ReloadIfChanged();
        QVERIFY( Store.WaitForIdle( 5000 ) );
        QCOMPARE( Changed.count(), 1 );
    }

    void dialogFilterOpenPinRemove()
    {
        QTemporaryDir Dir;
        QVERIFY( Dir.isValid() );
        RecentDocumentStore Store( Dir.path() );
        QVERIFY( Store.LoadSync() );
        const auto Base = QDateTime::currentDateTime().addSecs( -600 );
        Store.Record( Doc( Dir.filePath( "alpha.txt" ), "text", Base ) );
        Store.Record( Doc( Dir.filePath( "beta.md" ), "markdown", Base.addSecs( 1 ) ) );
        Store.Record( Doc( Dir.filePath( "gamma.pdf" ), "pdf", Base.addSecs( 2 ) ) );
        QVERIFY( Store.WaitForIdle( 5000 ) );

        RecentDocumentsDialog Dialog( &Store );
        Dialog.show();
        QVERIFY( QTest::qWaitForWindowExposed( &Dialog ) );
        auto* Search = Dialog.findChild< QLineEdit* >( "recentSearch" );
        auto* List   = Dialog.findChild< QListWidget* >( "recentList" );
        QVERIFY( Search && List );
        QCOMPARE( Search->placeholderText(), QStringLiteral( "최근 항목 검색" ) );
        QCOMPARE( VisibleItems( List ), 3 );

        Search->setText( "BETA" );
        QCOMPARE( VisibleItems( List ), 1 );
        QSignalSpy Open( &Dialog, &RecentDocumentsDialog::OpenRequested );
        QTest::keyClick( List, Qt::Key_Return );
        QCOMPARE( Open.count(), 1 );
        QCOMPARE( Open.at( 0 ).at( 0 ).value< RecentDocument >().Key(), RecentKey( Dir.filePath( "beta.md" ) ) );
        // 검색 입력의 Enter 는 기본 단추([열기])가 받는다.
        QTest::keyClick( Search, Qt::Key_Return );
        QCOMPARE( Open.count(), 2 );

        Search->clear();
        QCOMPARE( VisibleItems( List ), 3 );
        List->setCurrentRow( 2 );
        auto* Pin = Dialog.findChild< QPushButton* >( "recentPin" );
        QVERIFY( Pin );
        QCOMPARE( Pin->text(), QStringLiteral( "고정" ) );
        Pin->click();
        QVERIFY( Store.Entries().first().pinned_ );
        QCOMPARE( Store.Entries().first().Key(), RecentKey( Dir.filePath( "alpha.txt" ) ) );
        QCOMPARE( List->item( 0 )->data( RecentDocumentsDialog::DocumentRole ).value< RecentDocument >().Key(), RecentKey( Dir.filePath( "alpha.txt" ) ) );
        QCOMPARE( Pin->text(), QStringLiteral( "고정 해제" ) );

        List->setCurrentRow( 1 );
        QTest::keyClick( List, Qt::Key_Delete );
        QCOMPARE( Store.Entries().size(), 2 );
        QCOMPARE( List->count(), 2 );

        // 모두 지우기는 확인 후 고정 항목까지 지운다.
        auto* ClearAll = Dialog.findChild< QPushButton* >( "recentClearAll" );
        QVERIFY( ClearAll );
        ClearAll->click();
        QMessageBox* Confirm = nullptr;
        QTRY_VERIFY( ( Confirm = Dialog.findChild< QMessageBox* >( "recentClearConfirm" ) ) );
        Confirm->button( QMessageBox::Yes )->click();
        QTRY_VERIFY( Store.Entries().isEmpty() );
        QCOMPARE( List->count(), 0 );
        QVERIFY( Store.WaitForIdle( 5000 ) );
        Dialog.close();
    }

    void startPageRowsKeysAndSignals()
    {
        QTemporaryDir Dir;
        QVERIFY( Dir.isValid() );
        RecentDocumentStore Store( Dir.path() );
        QVERIFY( Store.LoadSync() );
        const auto Base = QDateTime::currentDateTime().addSecs( -600 );
        for( int Index = 0; Index < 7; ++Index ) Store.Record( Doc( Dir.filePath( QString( "doc%1.txt" ).arg( Index ) ), "text", Base.addSecs( Index ) ) );
        QVERIFY( Store.WaitForIdle( 5000 ) );

        DocumentStartPage Page( &Store );
        Page.resize( 800, 600 );
        Page.show();
        QVERIFY( QTest::qWaitForWindowExposed( &Page ) );
        QCOMPARE( Page.RowCount(), DocumentStartPage::MaxRows );
        QCOMPARE( VisibleRows( &Page ).size(), DocumentStartPage::MaxRows );

        QSignalSpy Open( &Page, &DocumentStartPage::OpenRequested );
        QTest::keyClick( &Page, Qt::Key_2 );
        QCOMPARE( Open.count(), 1 );
        QCOMPARE( Open.at( 0 ).at( 0 ).value< RecentDocument >().Key(), Store.Entries().at( 1 ).Key() );
        QTest::keyClick( &Page, Qt::Key_6 );
        QCOMPARE( Open.count(), 1 );
        QTest::keyClick( &Page, Qt::Key_5, Qt::KeypadModifier );
        QCOMPARE( Open.count(), 2 );
        QCOMPARE( Open.at( 1 ).at( 0 ).value< RecentDocument >().Key(), Store.Entries().at( 4 ).Key() );

        QSignalSpy OpenFile( &Page, &DocumentStartPage::OpenFileRequested );
        QSignalSpy NewDocument( &Page, &DocumentStartPage::NewDocumentRequested );
        QSignalSpy ShowAll( &Page, &DocumentStartPage::ShowAllRequested );
        auto* OpenButton    = Page.findChild< QAbstractButton* >( "startOpen" );
        auto* NewButton     = Page.findChild< QAbstractButton* >( "startNew" );
        auto* ShowAllButton = Page.findChild< QAbstractButton* >( "startShowAll" );
        QVERIFY( OpenButton && NewButton && ShowAllButton );
        QCOMPARE( OpenButton->text(), QStringLiteral( "열기…" ) );
        QCOMPARE( NewButton->text(), QStringLiteral( "새 문서" ) );
        QCOMPARE( ShowAllButton->text(), QStringLiteral( "모두 보기" ) );
        QTest::mouseClick( OpenButton, Qt::LeftButton );
        QTest::mouseClick( NewButton, Qt::LeftButton );
        QTest::mouseClick( ShowAllButton, Qt::LeftButton );
        QCOMPARE( OpenFile.count(), 1 );
        QCOMPARE( NewDocument.count(), 1 );
        QCOMPARE( ShowAll.count(), 1 );

        auto* Row = VisibleRows( &Page ).first();
        QTest::mouseClick( Row, Qt::LeftButton );
        QCOMPARE( Open.count(), 3 );
        QCOMPARE( Open.at( 2 ).at( 0 ).value< RecentDocument >().Key(), Store.Entries().first().Key() );

        // 빈 영역 메뉴(시안 MODES.empty.ctx.body).
        {
            std::unique_ptr< QMenu > Menu( Page.CreateContextMenu( QPoint( 2, 2 ) ) );
            QStringList Texts;
            for( auto* Action : Menu->actions() ) Texts.append( Action->isSeparator() ? QStringLiteral( "|" ) : Action->text() );
            QCOMPARE( Texts, ( QStringList{ "열기…", "새 문서", "|", "최근 항목 지우기" } ) );
            QVERIFY( Menu->actions().last()->isEnabled() );
        }
        // 행 메뉴: 열기 | 고정 · 목록에서 제거 | 경로 복사.
        {
            const auto Target = Store.Entries().at( 2 );
            auto*      Third  = VisibleRows( &Page ).at( 2 );
            std::unique_ptr< QMenu > Menu( Page.CreateContextMenu( Third->mapTo( &Page, Third->rect().center() ) ) );
            QStringList Texts;
            for( auto* Action : Menu->actions() ) Texts.append( Action->isSeparator() ? QStringLiteral( "|" ) : Action->text() );
            QCOMPARE( Texts, ( QStringList{ "열기", "|", "고정", "목록에서 제거", "|", "경로 복사" } ) );
            Menu->actions().at( 3 )->trigger();
            QVERIFY( !Contains( Store.Entries(), Target.path_ ) );
            QCOMPARE( Page.RowCount(), DocumentStartPage::MaxRows );
        }

        // 위 · 아래 · Enter.
        Page.activateWindow();
        if( QTest::qWaitForWindowActive( &Page ) ) {
            // 앞에서 누른 행이 포커스를 가졌으므로 시작 화면 자체로 되돌린 뒤 아래 키로 첫 행부터 간다.
            Page.setFocus( Qt::OtherFocusReason );
            QTRY_VERIFY( Page.hasFocus() );
            QTest::keyClick( &Page, Qt::Key_Down );
            QTest::keyClick( &Page, Qt::Key_Down );
            QTRY_VERIFY( VisibleRows( &Page ).at( 1 )->hasFocus() );
            QTest::keyClick( VisibleRows( &Page ).at( 1 ), Qt::Key_Return );
            QCOMPARE( Open.count(), 4 );
            QCOMPARE( Open.at( 3 ).at( 0 ).value< RecentDocument >().Key(), Store.Entries().at( 1 ).Key() );
        }

        // 로컬 파일 끌어다 놓기.
        QSignalSpy Dropped( &Page, &DocumentStartPage::FilesDropped );
        QMimeData Data;
        Data.setUrls( { QUrl::fromLocalFile( Dir.filePath( "dropped.txt" ) ), QUrl( "https://example.invalid/remote.txt" ) } );
        QDragEnterEvent Enter( QPoint( 10, 10 ), Qt::CopyAction, &Data, Qt::LeftButton, Qt::NoModifier );
        QApplication::sendEvent( &Page, &Enter );
        QVERIFY( Enter.isAccepted() );
        QDropEvent Drop( QPointF( 10, 10 ), Qt::CopyAction, &Data, Qt::LeftButton, Qt::NoModifier );
        QApplication::sendEvent( &Page, &Drop );
        QCOMPARE( Dropped.count(), 1 );
        QCOMPARE( Dropped.at( 0 ).at( 0 ).toStringList(), QStringList{ QDir::toNativeSeparators( Dir.filePath( "dropped.txt" ) ) } );

        // 목록이 비면 '최근 항목 없음' 을 보인다.
        Store.Clear( true );
        QCOMPARE( Page.RowCount(), 0 );
        QVERIFY( VisibleRows( &Page ).isEmpty() );
        QVERIFY( !Page.findChild< QWidget* >( "startRecentEmpty" )->isHidden() );
        QVERIFY( Store.WaitForIdle( 5000 ) );
    }
};

QTEST_MAIN( RecentDocumentsTests )
#include "tstRecentDocuments.moc"
