#pragma once

#include "esbonioLspClient.hpp"
#include "sphinxScanner.hpp"

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

namespace hc::rst {

/// 프로젝트마다 Esbonio 서버를 하나씩 띄워 관리한다.
///
/// 이 앱의 존재 이유가 여기다. Esbonio 는 한 폴더에 여러 Sphinx 프로젝트가
/// 있는 상황을 지원하지 않으므로, 프로젝트별로 서버를 따로 띄우고 열린 파일을
/// 알맞은 서버로 보낸다.
///
/// 클라이언트 하나는 **수명 내내 한 프로젝트만** 담당한다. 프로젝트가 바뀔
/// 때마다 하나를 죽였다 살리면 Sphinx 초기 빌드를 매번 다시 하게 된다.
class LspServerPool final : public QObject
{
    Q_OBJECT

public:
    explicit LspServerPool( QObject* parent = nullptr );
    ~LspServerPool() override;

    void                                setMaxProcesses( int count );
    [[nodiscard]] int                   maxProcesses() const;
    /// 서버 본체를 돌릴 인터프리터(항상 번들)와 sphinx-build 경로.
    void                                setPythonPaths( const QString& pythonExe, const QString& sphinxBuildExe );
    /// 다음 activate() 에서 띄울 서버가 sphinx_agent 에 쓸 인터프리터.
    /// 프로젝트마다 다를 수 있어 activate() 직전에 지정한다.
    void                                setSphinxPythonCommand( const QString& pythonExe );

    /// 활성 탭의 프로젝트는 절대 축출하지 않는다.
    /// activate() 보다 **먼저** 불러야 새 프로젝트를 띄우다가 지금 전환 중인
    /// 프로젝트를 밀어내는 일이 없다.
    void                                setPinnedProject( const QString& projectId );

    /// 해당 프로젝트의 클라이언트를 얻는다. 없으면 새로 띄운다(필요 시 축출).
    /// 최근 사용으로 표시한다.
    LspClient*                          activate( const SphinxProject& project );
    /// 조회만 한다. 없으면 nullptr.
    [[nodiscard]] LspClient*            clientFor( const QString& projectId ) const;
    [[nodiscard]] QStringList           runningProjectIds() const;

    void                                stopProject( const QString& projectId );
    void                                stopAll();

signals:
    // 모든 시그널이 projectId 를 달고 나간다. 단일 클라이언트와의 결정적 차이다.
    void                                logMessage( const QString& projectId, const QString& text );
    void                                diagnosticsReady( const QString& projectId, const QString& source,
                                                          const QString& path,
                                                          const QVector< DiagnosticEntry >& entries );
    void                                completionsReady( const QString& projectId, int requestId,
                                                          const QList< LspCompletionItem >& items );
    void                                documentSymbolsReady( const QString& projectId, const QString& path,
                                                              const QList< LspDocumentSymbol >& symbols );
    void                                serverNotification( const QString& projectId, const QString& method,
                                                            const QJsonObject& params );
    void                                projectSpawned( const QString& projectId );
    void                                projectEvicted( const QString& projectId );

private:
    void                                touch( const QString& projectId );
    void                                evictIfNeeded();

    QString                             pythonExe_;
    QString                             sphinxBuildExe_;
    QString                             sphinxPythonCommand_;
    QString                             pinnedProjectId_;
    int                                 maxProcesses_ = 3;

    /// front 가 가장 오래 전에 쓰인 것. N ≤ 8 이라 선형 탐색으로 충분하고
    /// 침입형 리스트보다 읽기 쉽다.
    QStringList                         lruOrder_;
    QHash< QString, LspClient* >        clients_;
};

}  // namespace hc::rst
