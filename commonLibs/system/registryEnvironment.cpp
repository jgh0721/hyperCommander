#include "stdafx.h"
#include "registryEnvironment.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
#include "registryAccess.hpp"
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryEnvironment, "hc.registry.environment")

namespace hc::registry {
EnvironmentExpansion ExpandEnvironmentText(const QString& Text, const QProcessEnvironment& Environment)
{
    EnvironmentExpansion Result;
    constexpr qsizetype Limit = WinRegistryBackend::MaxValueBytes / 2;
    if (Text.size() > Limit) { Result.tooLarge = true; return Result; }
    const auto Append = [&](QStringView Piece) {
        if (Piece.size() > Limit - Result.text.size()) { Result.tooLarge = true; return false; }
        Result.text.append(Piece); return true;
    };
    qsizetype Position = 0;
    while (Position < Text.size()) {
        const auto Open = Text.indexOf('%', Position);
        if (Open < 0) { Append(QStringView(Text).mid(Position)); break; }
        if (!Append(QStringView(Text).mid(Position, Open - Position))) break;
        const auto Close = Text.indexOf('%', Open + 1);
        if (Close < 0) { Append(QStringView(Text).mid(Open)); break; }
        const auto Name = Text.mid(Open + 1, Close - Open - 1);
        if (!Name.isEmpty() && Environment.contains(Name)) {
            if (!Append(Environment.value(Name))) break;
        } else {
            if (!Name.isEmpty() && !Result.missing.contains(Name, Qt::CaseInsensitive)) Result.missing.append(Name);
            if (!Append(QStringView(Text).mid(Open, Close - Open + 1))) break;
        }
        Position = Close + 1;
    }
    // Never display a truncated expansion as if it described the stored value.
    if (Result.tooLarge) Result.text.clear();
    return Result;
}
}
