#pragma once

#include "sphinxScanner.hpp"

#include <QHash>
#include <QObject>
#include <QString>

#include <memory>

class QTemporaryDir;

namespace hc::rst {

/// conf.py 가 없는 단독 .rst / .md 파일을 위한 임시 Sphinx 프로젝트.
///
/// Sphinx 는 conf.py 없이는 아무 것도 못 한다. 그래서 파일마다 임시 디렉터리에
/// 최소 conf.py 를 만들고, 그것을 confdir 로 삼는 가짜 SphinxProject 를
/// 합성한다. 그 뒤로는 프리뷰든 Esbonio 든 실제 프로젝트와 똑같이 다뤄진다.
///
/// srcdir 은 임시 디렉터리가 아니라 **원본 파일이 있는 실제 디렉터리**로 둔다.
/// 그래야 상대 경로 이미지/include 가 깨지지 않고, 진단도 실제 파일 경로로
/// 나와서 섀도 경로를 되돌리는 작업이 필요 없다.
/// 대신 include_patterns 로 그 파일 하나만 읽게 제한한다 — 그러지 않으면
/// 옆에 있는 무관한 문서까지 전부 빌드해 버린다.
class VirtualProjectManager final : public QObject
{
    Q_OBJECT

public:
    explicit VirtualProjectManager( QObject* parent = nullptr );
    ~VirtualProjectManager() override;

    /// 이 파일을 담당할 가상 프로젝트. 없으면 만든다.
    /// 만들 수 없으면 nullptr (임시 디렉터리 생성 실패 등).
    [[nodiscard]] const SphinxProject* projectFor( const QString& filePath );
    [[nodiscard]] const SphinxProject* findById( const QString& projectId ) const;

    /// 합성 conf.py 가 쓸 `html_theme`. 빈 문자열이면 alabaster 로 물러선다.
    ///
    /// 값이 바뀌면 **이미 만들어 둔 핸들을 전부 버린다** — conf.py 는 만들 때
    /// 한 번 쓰고 다시 읽지 않으므로, 남겨 두면 그 세션 동안 열려 있던 문서만
    /// 옛 테마로 렌더되어 "설정이 먹지 않는다" 로 보인다.
    void                                setHtmlTheme( const QString& theme );
    [[nodiscard]] QString               htmlTheme() const { return htmlTheme_; }

    /// 이 확장자를 가상 프로젝트로 다룰 것인가.
    [[nodiscard]] static bool           isSupported( const QString& filePath );
    [[nodiscard]] static QString        projectIdFor( const QString& filePath );

    void                                cleanup();

signals:
    void                                logMessage( const QString& text );

private:
    struct Handle
    {
        std::unique_ptr< QTemporaryDir > directory;   ///< 소멸 시 디렉터리 삭제
        SphinxProject                    project;
    };

    // QHash 는 복사 가능한 값을 요구하는데 Handle 은 unique_ptr 을 들고 있어
    // move-only 다. shared_ptr 로 감싼다.
    QHash< QString, std::shared_ptr< Handle > > handles_;   ///< projectId -> 핸들
    QString                             htmlTheme_;         ///< 빈 문자열 = alabaster
};

}  // namespace hc::rst
