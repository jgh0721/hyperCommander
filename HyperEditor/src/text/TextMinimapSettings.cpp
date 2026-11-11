#include "stdafx.h"
#include "text/TextMinimapSettings.h"

/// 모듈

/// UIs

/// 기타 헤더파일
#include "core/AppSettings.h"

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QtGlobal>

namespace
{
    const QString Prefix = QStringLiteral("textView/minimap/");

    QString sliderName(TextMinimapSettings::Slider Value)
    {
        return Value == TextMinimapSettings::Slider::Hover ? QStringLiteral("hover")
            : Value == TextMinimapSettings::Slider::Bracket ? QStringLiteral("bracket") : QStringLiteral("always");
    }
}

QString TextMinimapSettings::KindName(Kind Value)
{
    switch (Value) {
    case Kind::Document: return QStringLiteral("document");
    case Kind::Outline: return QStringLiteral("outline");
    case Kind::Overview: return QStringLiteral("overview");
    case Kind::Auto: default: return QStringLiteral("auto");
    }
}

TextMinimapSettings::Kind TextMinimapSettings::KindFromName(const QString& Name)
{
    if (Name == QLatin1String("document")) return Kind::Document;
    if (Name == QLatin1String("outline")) return Kind::Outline;
    if (Name == QLatin1String("overview")) return Kind::Overview;
    return Kind::Auto;
}

TextMinimapSettings TextMinimapSettings::Load()
{
    const AppSettings Stored;
    const auto Value = [&Stored](const char* Key) { return Stored.value(Prefix + QLatin1String(Key)); };
    TextMinimapSettings Result;
    Result.enabled_ = Value("enabled").toBool();
    Result.kind_ = KindFromName(Value("kind").toString());
    Result.side_ = Value("side").toString() == QLatin1String("left") ? Side::Left : Side::Right;
    Result.width_ = qBound(48, Value("width").toInt(), 220);
    Result.scale_ = Value("scale").toInt() == 2 ? 2 : 1;
    Result.blocks_ = Value("render").toString() == QLatin1String("blocks");
    const auto Slider = Value("slider").toString();
    Result.slider_ = Slider == QLatin1String("hover") ? Slider::Hover : Slider == QLatin1String("bracket") ? Slider::Bracket : Slider::Always;
    Result.search_ = Value("layers/search").toBool();
    Result.selection_ = Value("layers/selection").toBool();
    Result.changes_ = Value("layers/changes").toBool();
    Result.caret_ = Value("layers/caret").toBool();
    Result.problems_ = Value("layers/problems").toBool();
    Result.labels_ = Value("labels").toBool();
    Result.peek_ = Value("peek").toBool();
    Result.peekDelayMs_ = qBound(0, Value("peekDelayMs").toInt(), 5000);
    Result.largeLines_ = qMax(1, Value("largeLines").toInt());
    Result.largeBytes_ = qMax<qint64>(1, Value("largeBytes").toLongLong());
    Result.narrowWidth_ = qBound(0, Value("narrowWidth").toInt(), 4000);
    return Result;
}

void TextMinimapSettings::Store(const TextMinimapSettings& Settings)
{
    {
        AppSettings Stored;
        const auto Set = [&Stored](const char* Key, const QVariant& Value) { Stored.setValue(Prefix + QLatin1String(Key), Value); };
        Set("enabled", Settings.enabled_);
        Set("kind", KindName(Settings.kind_));
        Set("side", Settings.side_ == Side::Left ? QStringLiteral("left") : QStringLiteral("right"));
        Set("width", Settings.width_);
        Set("scale", Settings.scale_);
        Set("render", Settings.blocks_ ? QStringLiteral("blocks") : QStringLiteral("chars"));
        Set("slider", sliderName(Settings.slider_));
        Set("layers/search", Settings.search_);
        Set("layers/selection", Settings.selection_);
        Set("layers/changes", Settings.changes_);
        Set("layers/caret", Settings.caret_);
        Set("layers/problems", Settings.problems_);
        Set("labels", Settings.labels_);
        Set("peek", Settings.peek_);
        Set("peekDelayMs", Settings.peekDelayMs_);
        Set("largeLines", Settings.largeLines_);
        Set("largeBytes", Settings.largeBytes_);
        Set("narrowWidth", Settings.narrowWidth_);
    } // AppSettings writes on destruction, before anyone reloads.
    TextMinimapSettingsNotifier::Instance().Notify();
}

TextMinimapSettingsNotifier& TextMinimapSettingsNotifier::Instance()
{
    static TextMinimapSettingsNotifier Notifier;
    return Notifier;
}
