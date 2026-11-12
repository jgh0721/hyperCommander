#pragma once

#include "sphinxDiagnostics.hpp"

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace hc::rst {

/// 여러 출처의 진단을 모아 둔다.
///
/// 출처를 분리해 두는 게 핵심이다. Esbonio 는 파일 단위로
/// publishDiagnostics 를 보내고, sphinx-build 는 빌드 한 번에 전체를 보낸다.
/// 한 덩어리로 관리하면 프리뷰를 재빌드할 때마다 LSP 진단이 통째로 날아간다.
class DiagnosticsStore final : public QObject
{
    Q_OBJECT

public:
    explicit DiagnosticsStore( QObject* parent = nullptr );

    /// 한 출처의 진단을 통째로 교체한다 (sphinx-build 처럼 빌드 단위인 경우).
    void                                replaceSource( const QString& source,
                                                       const QVector< DiagnosticEntry >& entries );
    /// 한 출처 + 한 파일의 진단만 교체한다 (LSP publishDiagnostics 처럼 파일 단위).
    void                                replaceSourceForPath( const QString& source, const QString& path,
                                                              const QVector< DiagnosticEntry >& entries );
    /// 한 출처 + **여러 파일**의 진단을 한 번에 교체한다.
    ///
    /// `replaceSourceForPath` 를 반복해서 부르는 것과 최종 상태는 같지만,
    /// `changed()` 를 **한 번만** 낸다. 그 시그널이 진단 표 전체 재구축에 직접
    /// 이어져 있어서, 반복 호출은 표를 파일 수만큼 다시 만든다 — 문서 7개짜리
    /// 프로젝트에서도 빌드 한 번에 재구축 16회가 관측되었고, 처리 문서가 수십
    /// 개인 프로젝트에서는 그만큼 배가 된다.
    ///
    /// `paths` 는 이번 빌드가 **실제로 처리한** 파일 목록이다. 그 안에 있으면서
    /// `entriesByPath` 에 없는 파일은 "이제 깨끗하다" 는 뜻이므로 지운다 —
    /// 목록에 없는 파일은 건드리지 않는다(증분 빌드에서 다시 읽히지 않은 문서의
    /// 멀쩡한 진단을 지우지 않기 위한 기존 규칙 그대로다).
    void                                replacePathsForSource(
        const QString& source, const QStringList& paths,
        const QHash< QString, QVector< DiagnosticEntry > >& entriesByPath );
    void                                clearSource( const QString& source );
    void                                clear();

    /// 중복 제거된 전체 목록. 같은 위치면 Esbonio 가 sphinx-build 를 이긴다.
    [[nodiscard]] QVector< DiagnosticEntry > all() const;
    [[nodiscard]] QVector< DiagnosticEntry > forPath( const QString& path ) const;
    [[nodiscard]] int                   count() const;

signals:
    void                                changed();
    /// 특정 파일의 진단이 바뀌었다. 그 파일 편집기의 스퀴글만 갱신하면 된다.
    void                                pathChanged( const QString& path );

private:
    [[nodiscard]] static QString        normalizeKey( const QString& path );

    /// source -> (정규화된 경로 -> 진단들)
    QHash< QString, QHash< QString, QVector< DiagnosticEntry > > > bySource_;
};

}  // namespace hc::rst
