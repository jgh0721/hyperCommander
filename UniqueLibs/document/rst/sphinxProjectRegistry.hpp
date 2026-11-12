#pragma once

/// 모듈
#include "sphinxScanner.hpp"

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QObject>
#include <QString>

#include <vector>

namespace hc::rst {

/// 열어 본 문서들이 속한 Sphinx 프로젝트 목록.
///
/// MultiRoot reST Editor 는 워크스페이스 폴더를 통째로 훑어 conf.py 목록을 만들었다. HyperEditor 에는
/// 워크스페이스가 없다 — 파일을 하나씩 연다. 그래서 **파일에서 위로 올라가며 가장 가까운 conf.py** 를
/// 찾고, 찾은 프로젝트를 여기 쌓아 둔다. 같은 폴더 아래에 프로젝트가 여럿 있어도( DocA · DocB ) 파일마다
/// 제 프로젝트에 붙는다는 결과는 같다.
///
/// 디스크는 resolveForFile() 이 조상 폴더 몇 개의 conf.py 유무만 확인한다. 폴더를 훑지 않으므로 동기다.
class ProjectRegistry final : public QObject
{
    Q_OBJECT

public:
    /// 조상을 이만큼까지 올라가 본다.
    static constexpr int                MaxAncestorDepth = 24;

    explicit ProjectRegistry( QObject* parent = nullptr );
    ~ProjectRegistry() override;

    void                                setScannerSettings( ScannerSettings settings );
    [[nodiscard]] const ScannerSettings& scannerSettings() const;

    /// 폴더를 훑지 않으므로 언제나 거짓이다. 옮겨 온 조율 코드가 묻는다.
    [[nodiscard]] bool                  isScanning() const { return false; }

    [[nodiscard]] const std::vector< SphinxProject >& projects() const;

    /// 파일에 가장 가까운 프로젝트. 처음 보는 프로젝트면 목록에 넣는다. 없으면 nullptr( 가상 프로젝트 대상 ).
    /// 돌려준 포인터는 다음 resolveForFile() · rescanAsync() 전까지만 유효하다.
    [[nodiscard]] const SphinxProject*  resolveForFile( const QString& filePath );
    [[nodiscard]] const SphinxProject*  findById( const QString& projectId ) const;

    /// 알려진 프로젝트를 다시 확인한다: conf.py 가 사라진 것은 버리고, 남은 것은 conf.py 를 다시 읽는다
    /// ( 루트 문서 · myst · 빌드 폴더 설정이 바뀌었을 수 있다 ). 끝나면 scanFinished() — 받는 쪽이 열린
    /// 문서의 소속을 다시 정한다( 방금 conf.py 가 생겼다면 가상 프로젝트에서 실제 프로젝트로 옮겨 간다 ).
    void                                rescanAsync();

signals:
    void                                scanStarted();
    void                                scanFinished( int projectCount );
    void                                logMessage( const QString& text );

private:
    [[nodiscard]] SphinxProject         makeProject( const std::filesystem::path& confPath ) const;

    ScannerSettings                     settings_;
    std::vector< SphinxProject >        projects_;
};

}  // namespace hc::rst
