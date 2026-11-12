#include "rstTestRunner.hpp"

#include "sphinxDiagnosticsStore.hpp"

#include <QDir>
#include <QSignalSpy>
#include <QTest>

using namespace hc::rst;

/// 진단 저장소의 **배치 교체**가 반복 호출과 같은 상태를 만드는지 지킨다.
///
/// 왜 이 테스트가 필요한가. `changed()` 는 `MainWindow::refreshDiagnosticsTable`
/// 에 이어져 있고 그 함수는 표를 처음부터 다시 만든다 — 행마다
/// `QTableWidgetItem` 다섯 개와 `QFileInfo` 하나다. 예전에는 빌드 하나가
/// 처리 문서마다 `replaceSourceForPath` 를 불러 그 재구축을 문서 수만큼 반복했다
/// (문서 7개짜리 프로젝트에서도 빌드 한 번에 16회가 관측되었다).
///
/// 그래서 신호를 모으는 `replacePathsForSource` 를 넣었는데, 이런 종류의 변경은
/// **조용히 진단을 잃는** 방향으로 틀리기 쉽다. 그것이 바로 예전 주석이
/// "전체 교체하면 멀쩡한 진단이 사라진다" 며 파일 단위 교체를 택한 이유였다.
/// 그래서 여기서는 두 가지를 함께 못 박는다 — 신호 횟수와 **최종 상태의 동일성**.
class DiagnosticsStoreTest : public QObject
{
    Q_OBJECT

private slots:
    void batchEmitsChangedOnce();
    void batchMatchesRepeatedSingleCalls();
    void batchClearsOnlyListedPaths();
    void batchIgnoresEntriesOutsideTheList();
    void batchNotifiesEveryListedPath();
};

namespace {

/// 저장소는 절대 경로로 키를 만든다. 테스트가 상대 경로를 쓰면 실행 디렉터리에
/// 따라 값이 달라지므로 여기서 절대 경로로 맞춘다.
QString abs( const QString& name )
{
    return QDir::current().absoluteFilePath( name );
}

DiagnosticEntry entry( const QString& path, const int line, const QString& message,
                       const QString& source )
{
    DiagnosticEntry e;
    e.path = path;
    e.uri = pathToFileUri( path );
    e.line = line;
    e.severity = 1;
    e.message = message;
    e.source = source;
    return e;
}

/// 정렬된 (경로, 줄, 메시지) 목록. 두 경로가 만든 상태를 견주는 잣대다.
QStringList fingerprint( const QVector< DiagnosticEntry >& entries )
{
    QStringList out;
    out.reserve( entries.size() );
    for( const DiagnosticEntry& e : entries )
    {
        out << QStringLiteral( "%1|%2|%3|%4" )
                   .arg( QDir::fromNativeSeparators( e.path ).toCaseFolded() )
                   .arg( e.line )
                   .arg( e.message )
                   .arg( e.source );
    }
    out.sort();
    return out;
}

}   // namespace

void DiagnosticsStoreTest::batchEmitsChangedOnce()
{
    const QStringList paths{ abs( QStringLiteral( "a.rst" ) ), abs( QStringLiteral( "b.rst" ) ),
                             abs( QStringLiteral( "c.rst" ) ), abs( QStringLiteral( "d.rst" ) ) };

    QHash< QString, QVector< DiagnosticEntry > > byPath;
    for( const QString& path : paths )
        byPath[ path ] = { entry( path, 3, QStringLiteral( "경고" ), QStringLiteral( "sphinx" ) ) };

    DiagnosticsStore store;
    QSignalSpy changed( &store, &DiagnosticsStore::changed );
    QSignalSpy pathChanged( &store, &DiagnosticsStore::pathChanged );

    store.replacePathsForSource( QStringLiteral( "sphinx" ), paths, byPath );

    // 여기가 이 변경의 핵심이다 — 표 재구축은 문서 수와 무관하게 한 번이어야 한다.
    QCOMPARE( changed.count(), 1 );
    // 반면 파일별 통지는 그대로 남아야 한다. 편집기 스퀴글이 그것으로만 갱신된다.
    QCOMPARE( pathChanged.count(), paths.size() );
}

void DiagnosticsStoreTest::batchMatchesRepeatedSingleCalls()
{
    const QStringList paths{ abs( QStringLiteral( "a.rst" ) ), abs( QStringLiteral( "b.rst" ) ),
                             abs( QStringLiteral( "c.rst" ) ) };

    QHash< QString, QVector< DiagnosticEntry > > byPath;
    byPath[ paths.at( 0 ) ] = {
        entry( paths.at( 0 ), 1, QStringLiteral( "첫째" ), QStringLiteral( "sphinx" ) ),
        entry( paths.at( 0 ), 9, QStringLiteral( "둘째" ), QStringLiteral( "sphinx" ) ),
    };
    byPath[ paths.at( 2 ) ] = {
        entry( paths.at( 2 ), 4, QStringLiteral( "셋째" ), QStringLiteral( "sphinx" ) ),
    };
    // b.rst 는 목록에는 있으나 진단이 없다 = "이제 깨끗하다".

    // 다른 출처의 진단은 이 교체에 영향받지 않아야 한다. 그것이 출처를 나눠 둔 이유다.
    const QString esbonioPath = abs( QStringLiteral( "a.rst" ) );
    const QVector< DiagnosticEntry > esbonio{
        entry( esbonioPath, 7, QStringLiteral( "LSP 것" ), QStringLiteral( "esbonio" ) ) };

    DiagnosticsStore batched;
    batched.replaceSourceForPath( QStringLiteral( "esbonio" ), esbonioPath, esbonio );
    // 먼저 지난 빌드의 상태를 만들어 둔다 — 교체가 무엇을 지우는지 보려면 필요하다.
    batched.replaceSourceForPath( QStringLiteral( "sphinx" ), paths.at( 1 ),
                                  { entry( paths.at( 1 ), 2, QStringLiteral( "지난 것" ),
                                           QStringLiteral( "sphinx" ) ) } );

    DiagnosticsStore repeated;
    repeated.replaceSourceForPath( QStringLiteral( "esbonio" ), esbonioPath, esbonio );
    repeated.replaceSourceForPath( QStringLiteral( "sphinx" ), paths.at( 1 ),
                                   { entry( paths.at( 1 ), 2, QStringLiteral( "지난 것" ),
                                            QStringLiteral( "sphinx" ) ) } );

    batched.replacePathsForSource( QStringLiteral( "sphinx" ), paths, byPath );
    for( const QString& path : paths )
        repeated.replaceSourceForPath( QStringLiteral( "sphinx" ), path, byPath.value( path ) );

    QCOMPARE( fingerprint( batched.all() ), fingerprint( repeated.all() ) );
    QCOMPARE( batched.count(), repeated.count() );
    for( const QString& path : paths )
        QCOMPARE( fingerprint( batched.forPath( path ) ), fingerprint( repeated.forPath( path ) ) );
}

void DiagnosticsStoreTest::batchClearsOnlyListedPaths()
{
    const QString listed = abs( QStringLiteral( "listed.rst" ) );
    const QString untouched = abs( QStringLiteral( "untouched.rst" ) );

    DiagnosticsStore store;
    store.replaceSourceForPath(
        QStringLiteral( "sphinx" ), listed,
        { entry( listed, 1, QStringLiteral( "지울 것" ), QStringLiteral( "sphinx" ) ) } );
    store.replaceSourceForPath(
        QStringLiteral( "sphinx" ), untouched,
        { entry( untouched, 1, QStringLiteral( "남을 것" ), QStringLiteral( "sphinx" ) ) } );

    // 증분 빌드가 listed.rst 만 다시 읽고 아무 경고도 내지 않은 상황.
    store.replacePathsForSource( QStringLiteral( "sphinx" ), { listed }, {} );

    QVERIFY( store.forPath( listed ).isEmpty() );
    // 이 단언이 무너지면 증분 빌드마다 다른 문서의 멀쩡한 경고가 사라진다.
    QCOMPARE( store.forPath( untouched ).size(), 1 );
}

void DiagnosticsStoreTest::batchIgnoresEntriesOutsideTheList()
{
    const QString listed = abs( QStringLiteral( "listed.rst" ) );
    const QString stranger = abs( QStringLiteral( "stranger.rst" ) );

    QHash< QString, QVector< DiagnosticEntry > > byPath;
    byPath[ listed ] = { entry( listed, 1, QStringLiteral( "내 것" ), QStringLiteral( "sphinx" ) ) };
    byPath[ stranger ] = { entry( stranger, 1, QStringLiteral( "남의 것" ),
                                  QStringLiteral( "sphinx" ) ) };

    DiagnosticsStore store;
    QSignalSpy pathChanged( &store, &DiagnosticsStore::pathChanged );
    store.replacePathsForSource( QStringLiteral( "sphinx" ), { listed }, byPath );

    QCOMPARE( store.forPath( listed ).size(), 1 );
    // 처리 목록에 없는 파일은 저장하지 않는다. 예전 반복 호출도 그랬고
    // (grouped.value(key) 를 목록 순회로만 꺼냈다), 이 함수는 저장 규칙이 아니라
    // 신호 횟수만 바꾸는 것이 목적이다.
    QVERIFY( store.forPath( stranger ).isEmpty() );
    QCOMPARE( pathChanged.count(), 1 );
}

void DiagnosticsStoreTest::batchNotifiesEveryListedPath()
{
    // 진단이 없는 파일도 알려야 편집기 스퀴글이 지워진다. 있는 것만 알리면
    // 이미 고친 오류의 밑줄이 화면에 남는다.
    const QStringList paths{ abs( QStringLiteral( "one.rst" ) ), abs( QStringLiteral( "two.rst" ) ) };

    DiagnosticsStore store;
    QSignalSpy pathChanged( &store, &DiagnosticsStore::pathChanged );
    store.replacePathsForSource( QStringLiteral( "sphinx" ), paths, {} );

    QCOMPARE( pathChanged.count(), 2 );
}

HC_RST_REGISTER_TEST( DiagnosticsStoreTest );

#include "tstDiagnosticsStore.moc"
