#pragma once

#include "sphinxScanner.hpp"
#include "sphinxProjectRequirements.hpp"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

namespace hc::rst {

class PythonEnvManager;

// Q_NAMESPACE 는 네임스페이스당 한 번만 선언할 수 있고 solPythonEnvMgr.hpp 가
// 이미 갖고 있다. 이 enum 은 시그널/QVariant 로 넘기지 않으므로 등록이 필요 없다.
enum class EnvKind
{
    Bundled,           ///< 앱이 들고 다니는 런타임
    ProjectVenv,       ///< 프로젝트 근처에서 찾은 .venv / venv / env
    ExplicitOverride,  ///< 사용자가 직접 지정
};

/// 한 Sphinx 프로젝트에 대해 확정된 Python 실행 환경.
struct ResolvedPythonEnv
{
    QString                             projectKey;
    EnvKind                             kind = EnvKind::Bundled;
    QString                             pythonExe;
    QString                             originPath;   ///< 이 환경을 고른 근거 경로
    QString                             reason;       ///< 사람이 읽을 설명 (한국어)

    [[nodiscard]] bool                  isBundled() const { return kind == EnvKind::Bundled; }
    [[nodiscard]] QString               displayName() const;
};

/// 프로젝트의 Sphinx 를 돌릴 Python 을 알려 준다: **언제나 HyperEditor 가 uv 로 구성한 환경**이다.
///
/// MultiRoot reST Editor 는 프로젝트 근처의 가상환경( .venv 등 )이나 사용자가 지정한 인터프리터를 먼저
/// 썼다. HyperEditor 는 그러지 않는다 — 남의 가상환경은 실행하지 않고, 미리보기 빌드 · 프로젝트 빌드 ·
/// 언어 서버가 모두 같은 환경에서 돈다.
///
/// 프로젝트의 가상환경과 pyproject.toml · requirements 파일은 그 프로젝트가 쓰는 테마와 확장을 알아내는
/// 데만 쓴다( requirements() ). 빌드가 없다고 알린 것을 HyperEditor 환경에 설치할 때 프로젝트가 밝힌
/// 이름과 버전을 따른다.
class PythonEnvResolver final : public QObject
{
    Q_OBJECT

public:
    explicit PythonEnvResolver( PythonEnvManager* manager, QObject* parent = nullptr );

    [[nodiscard]] static QString        projectKeyFor( const std::filesystem::path& rootPath );

    /// 워크스페이스 루트를 알려주면 상향 탐색을 그 위로 올라가지 않는다.
    void                                setWorkspaceRoot( const QString& root );

    [[nodiscard]] ResolvedPythonEnv     resolve( const SphinxProject& project );
    void                                invalidate( const QString& projectKey );
    void                                invalidateAll();

    /// 프로젝트가 밝힌 테마 · 확장 요구( 선언 파일과 프로젝트 가상환경에서 읽는다 ). 프로젝트마다 한 번 읽어 둔다.
    [[nodiscard]] const ProjectRequirements& requirements( const SphinxProject& project );
    /// 프로젝트 근처의 가상환경 폴더( .venv · venv · env ). 없으면 빈 문자열. 실행하지 않고 읽기만 한다.
    [[nodiscard]] QString               projectVenvDir( const SphinxProject& project ) const;

    /// 파일 검사로 잡지 못한 손상을 실제 Python 실행 결과로 확인했을 때 호출한다.
    void                                reportRuntimeFailure( const SphinxProject& project,
                                                              const QString& pythonExe,
                                                              const QString& reason );
    /// 복구가 끝난 환경을 다시 탐색할 수 있게 손상 표시와 캐시를 지운다.
    void                                clearDamage( const QString& projectKey );

signals:
    void                                logMessage( const QString& text );
    void                                environmentDamaged( const QString& projectKey,
                                                            const QString& projectId,
                                                            const QString& environmentPath,
                                                            const QString& pythonExe,
                                                            const QString& reason );
    void                                environmentDamageCleared( const QString& projectKey );

private:
    [[nodiscard]] QString               findProjectVenv( const std::filesystem::path& rootPath,
                                                         QString* originPath ) const;

    PythonEnvManager*                   manager_ = nullptr;
    QString                             workspaceRoot_;
    QHash< QString, ResolvedPythonEnv > cache_;
    QHash< QString, ProjectRequirements > requirements_;
    QSet< QString >                     damagedKeys_;
};

}  // namespace hc::rst
