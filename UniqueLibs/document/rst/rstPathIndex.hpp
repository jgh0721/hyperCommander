#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <memory>

class QTimer;

namespace hc::rst {

struct PathIndexPublicationState;
using PathIndexChunk = std::shared_ptr< const QStringList >;
using PathIndexChunks = QVector< PathIndexChunk >;

/// 워크스페이스 루트 아래의 파일 목록을 한 번 훑어 들고 있는다.
///
/// 경로 자동완성의 "프로젝트 전역 퍼지" 후보가 여기서 나온다.
/// **뿌리는 srcdir 이 아니라 워크스페이스 루트다.** 실사용 워크스페이스를 재 보니
/// 이미지 907개 중 605개가 각 프로젝트의 srcdir **밖**에 있는 공유 트리에 있었고,
/// 문서들은 그것을 `../../../../Resources/...` 로 참조하고 있었다. srcdir 에
/// 인덱스를 걸면 정작 가장 많이 쓰는 후보가 구조적으로 하나도 안 나온다.
/// 덤으로 프로젝트가 30개여도 인덱스는 하나면 된다.
/// limit 가 양수면 그 개수에서 멈춘다. 0 이하는 전체를 수집한다.
[[nodiscard]] QStringList scanPathIndex( const QString& root, int limit = 0 );

class PathIndex final : public QObject
{
    Q_OBJECT

public:
    explicit PathIndex( QObject* parent = nullptr );
    ~PathIndex() override;

    /// 이 루트가 필요해졌다. 이미 훑었거나 훑는 중이면 아무것도 하지 않는다.
    void                                ensure( const QString& root );
    /// 다시 훑는다. 저장할 때마다 불리므로 짧은 간격은 스로틀로 막는다.
    void                                invalidate( const QString& root );
    /// 진행 중인 일을 취소하고 완성·부분 캐시를 모두 비운다.
    void                                clear();

    [[nodiscard]] bool                  isReadyFor( const QString& root ) const;
    [[nodiscard]] bool                  isScanning() const { return !scanningRoot_.isEmpty(); }
    [[nodiscard]] bool                  isScanningFor( const QString& root ) const;
    /// 루트 기준 `/` 구분 상대 경로. 정렬돼 있다.
    [[nodiscard]] const QStringList&    paths() const { return paths_; }
    [[nodiscard]] QString               indexedRoot() const { return indexedRoot_; }
    /// 지금 훑는 루트와 아직 정렬되지 않은 불변 경로 묶음. GUI 스레드에서만 읽는다.
    [[nodiscard]] QString               scanningRoot() const { return scanningRoot_; }
    [[nodiscard]] const PathIndexChunks& partialPathChunks() const {
        return partialPathChunks_;
    }
    [[nodiscard]] qsizetype             partialPathCount() const { return partialPathCount_; }
    [[nodiscard]] qsizetype             scannedPathCount() const { return scannedPathCount_; }

signals:
    /// 새 세대가 시작돼 부분 캐시가 비워졌다. 객체의(GUI) 스레드에서 난다.
    void                                scanStarted( const QString& root );
    /// 합쳐진 새 배치가 부분 캐시에 반영됐다. 이 신호는 항상 객체의(GUI)
    /// 스레드에서 난다. scannedCount는 발견한 전체 수로 partialPathCount()보다
    /// 클 수 있으며, 최종 ready snapshot이 생략된 진행 배치를 대체한다.
    void                                progress( const QString& root,
                                                  const QStringList& batch,
                                                  qsizetype scannedCount );
    /// 수집이 끝났다. 경로 후보를 띄우고 있으면 다시 채우는 데 쓴다.
    void                                ready( const QString& root, qsizetype count );
    void                                logMessage( const QString& text );

private:
    void                                startScan( const QString& root );
    void                                applyBatch( const QString& root, QStringList batch,
                                                    qsizetype scannedCount, quint64 generation );
    void                                drainPublishedBatches(
                                           const QString& root, quint64 generation,
                                           const std::shared_ptr< PathIndexPublicationState >& publication,
                                           const std::shared_ptr< std::atomic_bool >& cancellation );
    void                                apply( const QString& root, QStringList paths,
                                               quint64 generation );
    void                                applyCancelled( const QString& root, quint64 generation );
    void                                runPendingInvalidation();

    QString                             indexedRoot_;
    QString                             scanningRoot_;
    QStringList                         paths_;
    PathIndexChunks                     partialPathChunks_;
    qsizetype                           partialPathCount_ = 0;
    qsizetype                           scannedPathCount_ = 0;
    /// 늦게 도착한 이전 루트의 결과를 버리기 위한 세대 번호.
    quint64                             generation_ = 0;
    /// 이전 세대와 객체 소멸 때 워커의 트리 순회를 협조적으로 끊는다.
    std::shared_ptr< std::atomic_bool > scanCancellation_;
    /// 스캔 중이거나 스로틀 구간에 들어온 무효화를 한 번의 재스캔으로 합친다.
    QString                             pendingInvalidationRoot_;
    QTimer*                             rescanTimer_ = nullptr;
    /// 저장할 때마다 전 트리를 다시 훑지 않도록.
    QElapsedTimer                       sinceLastScan_;
};

}   // namespace hc::rst
