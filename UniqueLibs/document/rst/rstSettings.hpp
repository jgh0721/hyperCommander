#pragma once

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리
#include "core/AppSettings.h"

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QString>
#include <QVariant>

namespace hc::rst {

/// reStructuredText 지원의 설정. 제품의 통합 설정 파일에서 `documentViews/rst/…` 아래에 놓인다.
///
/// 키는 `rst/` 를 뺀 나머지로 준다( "applyUnsavedEdits", "python/useExternalUv" ). 저장된 값이 없으면
/// 제품 기본값( hc::defaults::edit::ViewValue ), 그것도 없으면 호출자가 준 기본값을 돌려준다.
///
/// 객체 하나가 설정 파일을 한 번 읽는다. 값을 여러 개 읽을 때는 객체 하나로 읽는다.
class Settings final
{
public:
    [[nodiscard]] QVariant value( const QString& Key, const QVariant& Default = {} ) const
    {
        const QVariant Stored = settings_.value( QStringLiteral( "rst/" ) + Key );
        return Stored.isValid() ? Stored : Default;
    }
    void setValue( const QString& Key, const QVariant& Value ) { settings_.setValue( QStringLiteral( "rst/" ) + Key, Value ); }
    void remove( const QString& Key ) { settings_.remove( QStringLiteral( "rst/" ) + Key ); }
    void sync() { settings_.sync(); }

private:
    AppSettings settings_;
};

}  // namespace hc::rst
