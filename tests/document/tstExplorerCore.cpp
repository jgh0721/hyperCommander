#include "UniqueLibs/document/explorer/explorerFilterWalker.hpp"
#include "UniqueLibs/document/explorer/explorerModel.hpp"
#include "UniqueLibs/document/explorer/explorerNames.hpp"
#include "UniqueLibs/document/explorer/remoteDrive.hpp"
#include "UniqueLibs/document/workspace/workspaceDiscovery.hpp"
#include "UniqueLibs/document/workspace/workspacePaths.hpp"
#include "UniqueLibs/document/workspace/workspaceState.hpp"
#include "UniqueLibs/document/documentShortcutRegistry.hpp"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeView>

// 탐색기 · 워크스페이스 모드의 화면 없는 핵심(설계 12절). MRE tst_FileTreeFilter · tst_ExternalFilesTree ·
// tst_WorkspaceSessionActive 의 뜻을 옮기고, 이 포팅에서 더한 규칙(제외 폴더 · 범위 · 상대 경로 · 대체 저장 ·
// MRE 가져오기 · 상위 찾기)을 본다. 실제 사용자 폴더는 건드리지 않고 QTemporaryDir 만 쓴다.
using namespace hc::document;
namespace
{
    bool Put( const QString& Path, const QByteArray& Bytes = {} )
    {
        QDir().mkpath( QFileInfo( Path ).absolutePath() );
        QFile File( Path );
        return File.open( QIODevice::WriteOnly ) && File.write( Bytes ) == Bytes.size();
    }
    QByteArray Json( const QJsonObject& Value ) { return QJsonDocument( Value ).toJson(); }

    // 원본 모델 + 필터 + 바깥 묶음을 트리에 건 탐색기 쌓임.
    struct Stack
    {
        explorer::FileSystemModel model_;
        explorer::FilterProxy filter_;
        explorer::OutsideProxy outside_;
        QTreeView tree_;
        Stack() { filter_.setSourceModel( &model_ ); outside_.setSourceModel( &filter_ ); tree_.setModel( &outside_ ); }
        void Root( const QString& Path )
        {
            const auto Source = model_.setRootPath( Path );
            filter_.SetRootSourceIndex( Source );
            filter_.SetScope( Path );
            outside_.SetRoot( Path, filter_.mapFromSource( Source ) );
            tree_.setRootIndex( outside_.mapFromSource( filter_.mapFromSource( Source ) ) );
        }
        QModelIndex View( const QString& Path ) { return outside_.mapFromSource( filter_.mapFromSource( model_.index( Path ) ) ); }
        bool Visible( const QString& Path ) { return View( Path ).isValid(); }
        // QFileSystemModel 은 폴더를 일꾼 스레드에서 읽는다: 읽으라고 하고 행이 생길 때까지 기다린다.
        void Load( const QString& Folder )
        {
            const auto Index = model_.index( Folder );
            if( model_.canFetchMore( Index ) ) model_.fetchMore( Index );
            QTRY_VERIFY_WITH_TIMEOUT( model_.rowCount( model_.index( Folder ) ) > 0, 5000 );
        }
    };
}

class ExplorerCoreTests : public QObject
{
    Q_OBJECT
private slots:
    void nameMatcher()
    {
        explorer::NameMatcher Matcher;
        QVERIFY( Matcher.Matches( "anything" ) );
        QVERIFY( Matcher.SetText( "  dock " ) );
        QVERIFY( !Matcher.SetText( "dock" ) );
        QVERIFY( Matcher.Matches( "Docking.rst" ) );
        QVERIFY( !Matcher.Matches( "index.rst" ) );
        Matcher.SetText( "*.rst" );
        QVERIFY( Matcher.IsWildcard() );
        QVERIFY( Matcher.Matches( "INDEX.RST" ) );
        // 와일드카드는 전체 일치: a.rst.bak 은 맞지 않는다(MRE).
        QVERIFY( !Matcher.Matches( "a.rst.bak" ) );
        QVERIFY( !Matcher.Matches( "" ) );
    }

    void entryNames()
    {
        QString Reason;
        for( const auto* Bad : { "", ".", "..", "name.", "name ", "a<b", "a:b", "a|b", "a?b", "a*b", "con", "COM1.txt" } ) {
            QVERIFY2( !explorer::IsSafeEntryName( QString::fromUtf8( Bad ), &Reason ), Bad );
            QVERIFY( !Reason.isEmpty() );
        }
        QVERIFY( explorer::IsSafeEntryName( QStringLiteral( "새 문서.rst" ) ) );
        QTemporaryDir Dir;
        QVERIFY( Put( Dir.filePath( "새 문서.md" ) ) );
        QCOMPARE( explorer::DefaultNewFileName( Dir.path(), false ), QStringLiteral( "새 문서 (2).md" ) );
        QCOMPARE( explorer::DefaultNewFileName( Dir.path(), true ), QStringLiteral( "새 문서.rst" ) );
        QVERIFY( !explorer::EntryNameProblem( Dir.path(), QStringLiteral( "새 문서.md" ) ).isEmpty() );
        // 이름 바꾸기에서 대소문자만 바꾸는 것은 허용한다.
        QVERIFY( explorer::EntryNameProblem( Dir.path(), QStringLiteral( "새 문서.MD" ), Dir.filePath( "새 문서.md" ) ).isEmpty() );
    }

    void driveRoots()
    {
        QCOMPARE( explorer::DriveRootOf( "d:\\Docs\\guide\\a.rst" ), QStringLiteral( "D:/" ) );
        QCOMPARE( explorer::DriveRootOf( "\\\\server\\share\\dir\\a.md" ), QStringLiteral( "//server/share/" ) );
        QVERIFY( explorer::IsUncPath( "//server/share/x" ) );
        QVERIFY( !explorer::IsDisconnectedRemoteDrivePath( QDir::tempPath() ) );
    }

    void filterKeepsAncestorsAndUnread()
    {
        QTemporaryDir Dir; const auto Root = Dir.path();
        Put( Root + "/guide/usage/docking.rst" ); Put( Root + "/guide/index.rst" ); Put( Root + "/api/reference.rst" ); Put( Root + "/README.md" );
        Stack S; S.Root( Root ); S.Load( Root ); S.Load( Root + "/guide" ); S.Load( Root + "/guide/usage" );
        S.filter_.SetFilterText( "dock" );
        QVERIFY( S.Visible( Root + "/guide" ) );
        QVERIFY( S.Visible( Root + "/guide/usage" ) );
        QVERIFY( S.Visible( Root + "/guide/usage/docking.rst" ) );
        QVERIFY( !S.Visible( Root + "/guide/index.rst" ) );
        QVERIFY( !S.Visible( Root + "/README.md" ) );
        // 아직 읽지 않은 폴더(api)는 남는다 — 지우면 그 안을 볼 길이 막힌다.
        QVERIFY( S.Visible( Root + "/api" ) );
        QVERIFY( S.filter_.IsMatch( S.model_.index( Root + "/guide/usage/docking.rst" ) ) );
        QVERIFY( !S.filter_.IsMatch( S.model_.index( Root + "/guide" ) ) );
        // 아무것도 맞지 않아도 뿌리는 남는다(뷰가 드라이브 전체로 튀지 않게).
        S.filter_.SetFilterText( "zzzz" );
        QVERIFY( S.tree_.rootIndex().isValid() );
        S.filter_.SetFilterText( {} );
        QVERIFY( S.Visible( Root + "/README.md" ) );
    }

    void excludedFoldersHidden()
    {
        QTemporaryDir Dir; const auto Root = Dir.path();
        Put( Root + "/.venv/lib/dock.py" ); Put( Root + "/node_modules/x.js" ); Put( Root + "/src/a.md" ); Put( Root + "/.multiroot/hypereditor.json", "{}" );
        Stack S; S.model_.SetShowHidden( true ); S.Root( Root ); S.Load( Root ); S.Load( Root + "/.venv" ); S.Load( Root + "/.venv/lib" );
        QVERIFY( !S.Visible( Root + "/.venv" ) );
        QVERIFY( !S.Visible( Root + "/node_modules" ) );
        QVERIFY( !S.Visible( Root + "/.multiroot" ) );
        QVERIFY( S.Visible( Root + "/src" ) );
        // 이름이 맞는 자손도 제외 폴더를 되살리지 못한다.
        S.filter_.SetFilterText( "dock" );
        QVERIFY( !S.Visible( Root + "/.venv" ) );
        S.filter_.SetFilterText( {} );
        S.filter_.SetShowHidden( true );
        QVERIFY( S.Visible( Root + "/.venv" ) );
    }

    void scopeLimitsFilter()
    {
        QTemporaryDir Dir; const auto Root = Dir.path();
        Put( Root + "/doc/a.md" ); Put( Root + "/other/b.md" ); Put( Root + "/other/deep/c.md" );
        Stack S; S.Root( Root ); S.Load( Root );
        S.filter_.SetScope( Root + "/doc" );
        S.filter_.SetFilterText( ".md" );
        // 범위 밖의 읽지 않은 폴더는 필터 중에 남지 않는다(문서 모드에서 드라이브 전체를 훑지 않게).
        QVERIFY( !S.Visible( Root + "/other" ) );
        QVERIFY( S.Visible( Root + "/doc" ) );
        QVERIFY( S.filter_.IsInScope( Root + "/doc/a.md" ) );
        QVERIFY( !S.filter_.IsInScope( Root + "/other/b.md" ) );
        S.filter_.SetFilterText( {} );
        QVERIFY( S.Visible( Root + "/other" ) );
    }

    void walkerStaysInScope()
    {
        QTemporaryDir Dir; const auto Root = Dir.path();
        for( int At = 0; At < 6; ++At ) Put( Root + QString( "/doc/d%1/f%1.md" ).arg( At ) );
        Put( Root + "/elsewhere/x/y.md" );
        Stack S; S.Root( Root ); S.Load( Root );
        S.filter_.SetScope( Root + "/doc" );
        S.filter_.SetFilterText( "f5" );
        explorer::FilterWalker Walker( &S.tree_, &S.model_,
            [ &S ]( const QModelIndex& Source ) { return S.outside_.mapFromSource( S.filter_.mapFromSource( Source ) ); },
            [ &S ]( const QModelIndex& View ) { return S.filter_.mapToSource( S.outside_.mapToSource( View ) ); } );
        Walker.Start( S.model_.index( Root + "/doc" ) );
        QTRY_VERIFY_WITH_TIMEOUT( S.Visible( Root + "/doc/d5/f5.md" ), 10000 );
        QVERIFY( !S.Visible( Root + "/doc/d1/f1.md" ) );
        // 범위 밖 폴더는 읽지 않는다.
        QVERIFY( S.model_.canFetchMore( S.model_.index( Root + "/elsewhere" ) ) || S.model_.rowCount( S.model_.index( Root + "/elsewhere" ) ) == 0 );
        Walker.Stop();
    }

    void outsideGroup()
    {
        QTemporaryDir Dir, Other; const auto Root = Dir.path();
        Put( Root + "/in.md" ); Put( Other.filePath( "todo.md" ) ); Put( Other.filePath( "memo.txt" ) );
        Stack S; S.Root( Root ); S.Load( Root );
        S.outside_.SetGroupLabel( QStringLiteral( "워크스페이스 밖" ) );
        QCOMPARE( S.outside_.SetFiles( { Root + "/in.md", Other.filePath( "todo.md" ), Other.filePath( "TODO.md" ), {} } ), 1 );
        const auto Group = S.outside_.GroupIndex();
        QVERIFY( Group.isValid() );
        QCOMPARE( Group.parent(), S.tree_.rootIndex() );
        QCOMPARE( Group.data().toString(), QStringLiteral( "워크스페이스 밖" ) );
        QCOMPARE( S.outside_.rowCount( Group ), 1 );
        QVERIFY( S.outside_.IsOutsideFile( S.outside_.index( 0, 0, Group ) ) );
        QVERIFY( workspace::SamePath( S.outside_.FilePath( S.outside_.index( 0, 0, Group ) ), Other.filePath( "todo.md" ) ) );
        // 통째로 바뀐다: 탭을 닫은 문서는 빠지고 새 문서는 끝에 붙는다.
        S.outside_.SetFiles( { Other.filePath( "memo.txt" ) } );
        QCOMPARE( S.outside_.rowCount( S.outside_.GroupIndex() ), 1 );
        QCOMPARE( S.outside_.index( 0, 0, S.outside_.GroupIndex() ).data().toString(), QStringLiteral( "memo.txt" ) );
        // 필터는 묶음에도 건다.
        S.filter_.SetFilterText( "todo" );
        QVERIFY( !S.outside_.GroupIndex().isValid() );
        S.filter_.SetFilterText( {} );
        S.outside_.SetFiles( {} );
        QVERIFY( !S.outside_.GroupIndex().isValid() );
    }

    void outsideGroupAtComputerRoot()
    {
        QTemporaryDir Other; Put( Other.filePath( "memo.txt" ) );
        explorer::FileSystemModel Model; explorer::FilterProxy Filter; explorer::OutsideProxy Outside;
        Filter.setSourceModel( &Model ); Outside.setSourceModel( &Filter );
        Model.setRootPath( QString() );
        Outside.SetRoot( {}, {} );
        Outside.SetFiles( { Other.filePath( "memo.txt" ) } );
        const auto Group = Outside.GroupIndex();
        QVERIFY( Group.isValid() );
        QVERIFY( !Group.parent().isValid() );
        QCOMPARE( Group.row(), Filter.rowCount() );
    }

    void allowedTopLevelDrive()
    {
        explorer::FileSystemModel Model; explorer::FilterProxy Filter; Filter.setSourceModel( &Model );
        Model.setRootPath( QString() );
        QTRY_VERIFY( Model.rowCount() > 0 );
        const auto Drive = explorer::DriveRootOf( QDir::tempPath() );
        Filter.SetAllowedTopLevelRoots( { Drive } );
        QCOMPARE( Filter.rowCount(), 1 );
        QVERIFY( workspace::SamePath( Filter.SourcePath( Filter.mapToSource( Filter.index( 0, 0 ) ) ), Drive ) );
        Filter.SetAllowedTopLevelRoots( {} );
        QVERIFY( Filter.rowCount() >= 1 );
    }

    void storedPaths()
    {
        QCOMPARE( workspace::ToStored( "D:/Docs", "d:\\Docs\\guide\\index.rst" ), QStringLiteral( "guide/index.rst" ) );
        bool Outside = false;
        QCOMPARE( workspace::ToStored( "D:/Docs", "E:/Notes/todo.md", &Outside ), QStringLiteral( "E:/Notes/todo.md" ) );
        QVERIFY( Outside );
        // 상대 경로는 지금 루트 기준 — 폴더를 옮겨도 따라간다.
        QCOMPARE( workspace::FromStored( "E:/Moved/Docs", "guide/index.rst" ), QStringLiteral( "E:/Moved/Docs/guide/index.rst" ) );
        QVERIFY( workspace::IsInside( "D:/DOCS/a.md", "d:/docs", false ) );
        QVERIFY( !workspace::IsInside( "D:/Docs2/a.md", "D:/Docs", false ) );
        const auto Ancestors = workspace::AncestorFolders( "D:/Docs/guide/usage" );
        QCOMPARE( Ancestors.first(), QStringLiteral( "D:/Docs/guide/usage" ) );
        QCOMPARE( Ancestors.last(), QStringLiteral( "D:/" ) );
    }

    void stateRoundTripAndMove()
    {
        QTemporaryDir Profile, First, Second;
        workspace::Store Store( Profile.path() );
        workspace::State Value;
        workspace::DocumentEntry Doc; Doc.path_ = First.filePath( "guide/index.rst" ); Doc.kind_ = "rst";
        workspace::Position Where; Where.anchor_ = 10; Where.caret_ = 12; Where.firstLine_ = 3; Doc.position_ = Where;
        workspace::DocumentEntry Away; Away.path_ = Profile.filePath( "todo.md" );
        Value.documents_ = { Doc, Away };
        Value.active_ = Doc.path_;
        Value.explorer_.expanded_ = { First.filePath( "guide" ) };
        Value.dockLayout_ = "layout"; Value.dockVersion_ = 2;
        Value.extra_[ "future" ] = 7;
        const auto Saved = Store.Save( First.path(), Value, workspace::Location::Folder );
        QVERIFY( Saved.ok_ && !Saved.fellBack_ );
        QVERIFY( QFile::exists( workspace::Store::FolderStatePath( First.path() ) ) );
        // 폴더째 옮긴다.
        QFile Copy( workspace::Store::FolderStatePath( First.path() ) );
        QDir().mkpath( Second.filePath( ".multiroot" ) );
        QVERIFY( Copy.copy( workspace::Store::FolderStatePath( Second.path() ) ) );
        const auto Loaded = Store.Load( Second.path() );
        QVERIFY( Loaded.found_ );
        QCOMPARE( Loaded.state_.documents_.size(), 2 );
        QVERIFY( workspace::SamePath( Loaded.state_.documents_.first().path_, Second.filePath( "guide/index.rst" ) ) );
        QVERIFY( Loaded.state_.documents_.last().outside_ );
        QCOMPARE( Loaded.state_.documents_.first().position_->caret_, 12 );
        QVERIFY( workspace::SamePath( Loaded.state_.active_, Second.filePath( "guide/index.rst" ) ) );
        QVERIFY( workspace::SamePath( Loaded.state_.explorer_.expanded_.value( 0 ), Second.filePath( "guide" ) ) );
        QCOMPARE( Loaded.state_.dockLayout_, QByteArray( "layout" ) );
        // 모르는 키는 다시 쓸 때도 남는다.
        QCOMPARE( Loaded.state_.extra_[ "future" ].toInt(), 7 );
        QVERIFY( Store.Save( Second.path(), Loaded.state_, workspace::Location::Folder ).ok_ );
        QFile Reread( workspace::Store::FolderStatePath( Second.path() ) ); QVERIFY( Reread.open( QIODevice::ReadOnly ) );
        QCOMPARE( QJsonDocument::fromJson( Reread.readAll() ).object()[ "future" ].toInt(), 7 );
    }

    void newerSchemaNotOverwritten()
    {
        QTemporaryDir Profile, Root;
        QVERIFY( Put( workspace::Store::FolderStatePath( Root.path() ), Json( { { "schema", 2 }, { "documents", QJsonArray{} } } ) ) );
        workspace::Store Store( Profile.path() );
        QVERIFY( Store.Load( Root.path() ).state_.newerSchema_ );
        const auto Result = Store.Save( Root.path(), {}, workspace::Location::Folder );
        QVERIFY( !Result.ok_ );
        QCOMPARE( Result.error_, QStringLiteral( "newer-schema" ) );
    }

    void fallbackToProfile()
    {
        QTemporaryDir Profile, Root;
        // .multiroot 가 파일이면 mkpath 가 실패한다(읽기 전용 폴더와 같은 결과).
        QVERIFY( Put( Root.filePath( ".multiroot" ), "x" ) );
        workspace::Store Store( Profile.path() );
        workspace::State Value; workspace::DocumentEntry Doc; Doc.path_ = Root.filePath( "a.md" ); Value.documents_ = { Doc };
        const auto Saved = Store.Save( Root.path(), Value, workspace::Location::Folder );
        QVERIFY( Saved.ok_ && Saved.fellBack_ );
        QCOMPARE( Saved.to_, workspace::Location::Profile );
        QVERIFY( Store.ProfileRoots().contains( workspace::NormalizeRoot( Root.path() ) ) );
        const auto Loaded = Store.Load( Root.path() );
        QVERIFY( Loaded.found_ );
        QCOMPARE( Loaded.from_, workspace::Location::Profile );
    }

    void newestOfTwo()
    {
        QTemporaryDir Profile, Root;
        workspace::Store Store( Profile.path() );
        workspace::State Old; Old.savedAt_ = QDateTime::currentDateTime().addDays( -1 ); workspace::DocumentEntry A; A.path_ = Root.filePath( "old.md" ); Old.documents_ = { A };
        workspace::State New; New.savedAt_ = QDateTime::currentDateTime(); workspace::DocumentEntry B; B.path_ = Root.filePath( "new.md" ); New.documents_ = { B };
        QVERIFY( Store.Save( Root.path(), Old, workspace::Location::Folder ).ok_ );
        QVERIFY( Store.Save( Root.path(), New, workspace::Location::Profile ).ok_ );
        const auto Loaded = Store.Load( Root.path() );
        QCOMPARE( Loaded.from_, workspace::Location::Profile );
        QVERIFY( Loaded.state_.documents_.first().path_.endsWith( "new.md" ) );
    }

    void importMultiRoot()
    {
        // 저장소를 옮기기 전 위치가 적힌 MRE 세션(이 저장소의 documentation/.multiroot 와 같은 모양).
        QTemporaryDir Root;
        const QJsonObject Session{ { "schema", 1 }, { "workspaceRoot", "D:/Dev/hyperCommander/documentation" }, { "activeIndex", 1 },
            { "dockLayout", "AAAA" }, { "windowGeometry", "BBBB" }, { "externalFiles", QJsonArray{ "C:/x/ext.md" } },
            { "documents", QJsonArray{
                QJsonObject{ { "path", "D:/Dev/hyperCommander/documentation/source/a.rst" }, { "caretLine", 40 }, { "caretColumn", 3 }, { "firstVisibleLine", 30 } },
                QJsonObject{ { "path", "D:/Dev/HyperCommander/documentation/source/b.rst" }, { "caretLine", 2 }, { "caretColumn", 1 }, { "firstVisibleLine", 1 } },
                QJsonObject{ { "path", "C:/Elsewhere/c.md" }, { "caretLine", 1 }, { "caretColumn", 1 }, { "firstVisibleLine", 1 } } } } };
        QVERIFY( Put( workspace::MultiRootSessionPath( Root.path() ), Json( Session ) ) );
        const auto Imported = workspace::ImportMultiRoot( Root.path() );
        QVERIFY( Imported.has_value() );
        QCOMPARE( Imported->documents_.size(), 3 );
        QVERIFY( workspace::SamePath( Imported->documents_[ 0 ].path_, Root.filePath( "source/a.rst" ) ) );
        QVERIFY( workspace::SamePath( Imported->documents_[ 1 ].path_, Root.filePath( "source/b.rst" ) ) );
        QVERIFY( Imported->documents_[ 2 ].outside_ );
        QCOMPARE( Imported->documents_[ 0 ].position_->caretLine_, 40 );
        QCOMPARE( Imported->documents_[ 0 ].position_->firstLine_, 29 );
        QVERIFY( workspace::SamePath( Imported->active_, Root.filePath( "source/b.rst" ) ) );
        QVERIFY( Imported->dockLayout_.isEmpty() && Imported->geometry_.isEmpty() );
        // 다른 판은 가져오지 않는다.
        QVERIFY( Put( workspace::MultiRootSessionPath( Root.path() ), Json( { { "schema", 2 } } ) ) );
        QVERIFY( !workspace::ImportMultiRoot( Root.path() ).has_value() );
    }

    void projectsCache()
    {
        QTemporaryDir Root;
        Put( Root.filePath( "guide/conf.py" ) );
        const workspace::ProjectRecord Live{ "guide", Root.filePath( "guide" ), Root.filePath( "guide/conf.py" ), Root.filePath( "guide" ), "index", Root.filePath( "guide/_build" ), false };
        const workspace::ProjectRecord Gone{ "gone", Root.filePath( "gone" ), Root.filePath( "gone/conf.py" ), Root.filePath( "gone" ), "index", {}, true };
        QVERIFY( workspace::WriteProjectsCache( Root.path(), { Live, Gone } ) );
        const auto Read = workspace::ReadProjectsCache( Root.path() );
        QCOMPARE( Read.size(), 1 );
        QCOMPARE( Read.first(), Live );
        QFile File( workspace::ProjectsCachePath( Root.path() ) ); QVERIFY( File.open( QIODevice::ReadOnly ) );
        QCOMPARE( QJsonDocument::fromJson( File.readAll() ).object()[ "version" ].toInt(), 2 );
    }

    void ancestorCandidates()
    {
        workspace::MarkerCache::Instance().Clear();
        QTemporaryDir Dir; const auto Root = Dir.path();
        const auto File = Root + "/Docs/guide/usage/docking.rst";
        Put( File );
        // 0개: 아무 표시도 없다.
        QCOMPARE( workspace::FindAncestorWorkspaces( File, {} ).candidates_.size(), 0 );
        // projects.json 만 있는 .multiroot 는 후보가 아니다.
        Put( Root + "/Docs/guide/.multiroot/projects.json", "{}" );
        QCOMPARE( workspace::FindAncestorWorkspaces( File, {} ).candidates_.size(), 0 );
        // 1개: D:\Docs 의 HyperEditor 상태.
        Put( Root + "/Docs/.multiroot/hypereditor.json", Json( { { "schema", 1 }, { "savedAt", "2026-11-13T10:00:00+09:00" }, { "documents", QJsonArray{ QJsonObject{}, QJsonObject{} } } } ) );
        workspace::MarkerCache::Instance().Clear();
        auto Found = workspace::FindAncestorWorkspaces( File, {} );
        QCOMPARE( Found.candidates_.size(), 1 );
        QCOMPARE( Found.candidates_.first().kind_, workspace::Candidate::Kind::HyperEditor );
        QCOMPARE( Found.candidates_.first().documentCount_, 2 );
        // 2개: guide 에 MRE 세션이 생기면 가까운 것부터.
        Put( Root + "/Docs/guide/.multiroot/workspace.json", Json( { { "schema", 1 }, { "documents", QJsonArray{ QJsonObject{} } } } ) );
        workspace::MarkerCache::Instance().Clear();
        Found = workspace::FindAncestorWorkspaces( File, {} );
        QCOMPARE( Found.candidates_.size(), 2 );
        QVERIFY( workspace::SamePath( Found.candidates_[ 0 ].root_, Root + "/Docs/guide" ) );
        QCOMPARE( Found.candidates_[ 0 ].kind_, workspace::Candidate::Kind::MultiRoot );
        QVERIFY( workspace::SamePath( Found.candidates_[ 1 ].root_, Root + "/Docs" ) );
        // 알려진 루트(프로필 · 최근)는 표시가 없어도 후보이고, 같은 루트는 한 번만 센다.
        Found = workspace::FindAncestorWorkspaces( File, { Root + "/Docs/guide/usage", Root + "/DOCS" } );
        QCOMPARE( Found.candidates_.size(), 3 );
        QCOMPARE( Found.candidates_.first().kind_, workspace::Candidate::Kind::Known );
        workspace::MarkerKind Kind{};
        QVERIFY( workspace::MarkerCache::Instance().Peek( Root + "/Docs", &Kind ) );
        QCOMPARE( Kind, workspace::MarkerKind::HyperEditor );
    }

    void registryDeepestAndAutoUnbind()
    {
        auto& Registry = workspace::Registry::Instance();
        auto* Outer = new QObject; auto* Inner = new QObject;
        QVERIFY( Registry.Bind( "D:/Docs", Outer ) );
        QVERIFY( Registry.Bind( "D:/Docs/guide", Inner ) );
        // 같은 폴더는 한 창에만.
        QObject Third;
        QVERIFY( !Registry.Bind( "d:\\docs", &Third ) );
        QString Root;
        QCOMPARE( Registry.WindowContaining( "D:/Docs/guide/usage/a.rst", &Root ), Inner );
        QCOMPARE( Root, QStringLiteral( "D:/Docs/guide" ) );
        QCOMPARE( Registry.WindowContaining( "D:/Docs/api/b.rst" ), Outer );
        QCOMPARE( Registry.WindowContaining( "E:/x.md" ), nullptr );
        delete Inner;
        QCOMPARE( Registry.WindowContaining( "D:/Docs/guide/usage/a.rst" ), Outer );
        QCOMPARE( Registry.RootOf( Outer ), QStringLiteral( "D:/Docs" ) );
        Registry.Unbind( Outer );
        QCOMPARE( Registry.WindowFor( "D:/Docs" ), nullptr );
        delete Outer;
    }

    // 탐색기 · 워크스페이스 명령의 기본 키가 다른 범위와 겹치지 않는다(Enter 는 포커스 기본키라 트리가 직접 받는다).
    void shortcutDefaults()
    {
        QCOMPARE( DocumentShortcutRegistry::Validate( {} ), QString() );
        QCOMPARE( DocumentShortcutRegistry::ScopeOf( "explorer.rename" ), QStringLiteral( "explorer" ) );
        QVERIFY( DocumentShortcutRegistry::CoActive( "explorer", "common" ) );
        QVERIFY( !DocumentShortcutRegistry::CoActive( "explorer", "edit" ) );
        QVERIFY( hc::defaults::edit::ViewValue( "workspace/stateLocation" ).toString() == QStringLiteral( "folder" ) );
    }
};

QTEST_MAIN( ExplorerCoreTests )
#include "tstExplorerCore.moc"
