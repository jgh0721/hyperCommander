#pragma once

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QObject>
#include <QString>

// Minimap preferences, stored under textView/minimap/ in the documentViews section. Defaults live in
// hc::defaults::edit::ViewValues(); AppSettings falls back to them for keys that were never written.
struct TextMinimapSettings
{
    enum class Kind { Auto, Document, Outline, Overview };
    enum class Side { Right, Left };
    enum class Slider { Always, Hover, Bracket };

    bool enabled_ = true;
    Kind kind_ = Kind::Auto;
    Side side_ = Side::Right;
    int width_ = 104;
    int scale_ = 1;
    bool blocks_ = false;
    Slider slider_ = Slider::Always;
    bool search_ = true;
    bool selection_ = true;
    bool changes_ = true;
    bool caret_ = true;
    bool problems_ = true;
    bool labels_ = true;
    bool peek_ = true;
    int peekDelayMs_ = 300;
    int largeLines_ = 20000;
    qint64 largeBytes_ = 16LL * 1024 * 1024;
    int narrowWidth_ = 480;

    static TextMinimapSettings Load();
    // Writes every field and tells all open editors.
    static void Store(const TextMinimapSettings& Settings);
    static QString KindName(Kind Value);
    static Kind KindFromName(const QString& Name);
};

// One notifier per process: a change made in one editor (menu, shortcut, minimap context menu) reaches every editor
// at once instead of waiting for the next settings reload.
class TextMinimapSettingsNotifier final : public QObject
{
    Q_OBJECT

public:
    static TextMinimapSettingsNotifier& Instance();
    void Notify() { emit Changed(); }

signals:
    void Changed();
};
