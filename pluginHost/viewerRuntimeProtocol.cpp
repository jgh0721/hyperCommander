#include "stdafx.h"
#include "viewerHostRuntime.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QJsonDocument>

namespace hc::plugins
{
    bool IsRuntimeCommand(WireKind Kind)
    {
        return Kind == WireKind::Hello || Kind == WireKind::Ping || Kind == WireKind::Probe || Kind == WireKind::PluginConfigure
            || IsViewerCommand(Kind) || Kind == WireKind::MetadataQuery || Kind == WireKind::ContentConvert;
    }
    bool RuntimeIdentity(const QString& Value)
    {
        if (Value.size() != 32) return false;
        for (const auto Ch : Value) if (!((Ch >= '0' && Ch <= '9') || (Ch >= 'a' && Ch <= 'f'))) return false;
        return true;
    }
    QByteArray RuntimeJson(const QJsonObject& Value) { return QJsonDocument(Value).toJson(QJsonDocument::Compact); }
    storage::Result<QJsonObject> RuntimeObject(const QByteArray& Value)
    {
        QJsonParseError Error; const auto Document = QJsonDocument::fromJson(Value, &Error);
        if (Error.error != QJsonParseError::NoError || !Document.isObject()) return HostError("host-runtime-json");
        return Document.object();
    }
}
