#pragma once

#include <QObject>
#include <QWidget>
#include <QColor>
#include <QHash>
#include <QList>
#include <QString>

/// 전역 테마 관리자 (싱글톤)
class ThemeManager : public QObject
{
    Q_OBJECT

public:
    enum Theme { Light, Dark };
    Q_ENUM(Theme)

    struct ColorEntry {
        QString key;
        QString label;
        QString group;
    };

    static ThemeManager& instance();

    Theme currentTheme() const;
    void  setTheme(Theme theme);

    QColor color(const QString& key) const;
    bool hasColor(const QString& key) const;
    bool hasColorOverride(const QString& key) const;
    void setColorOverride(const QString& key, const QColor& color);
    void setColorOverrides(const QHash<QString, QColor>& colors);
    void resetColorOverrides(Theme theme);
    QHash<QString, QColor> colorOverrides(Theme theme) const;
    QHash<QString, QColor> effectiveColors(Theme theme) const;
    void ReloadColorOverrides();

    static QList<ColorEntry> editableColorEntries();
    static QHash<QString, QColor> defaultColors(Theme theme);
    static QString themeName(Theme theme);

    bool importThemeFile(const QString& filePath, QString* errorMessage = nullptr);
    bool exportThemeFile(const QString& filePath, QString* errorMessage = nullptr) const;

    // 테마별 색상 조회
    QColor backgroundColor() const;
    QColor foregroundColor() const;
    QColor toolBarColor() const;
    QColor tabActiveColor() const;
    QColor tabInactiveColor() const;
    QColor canvasAreaColor() const;

    // 앱 전체 스타일시트 적용
    void applyToApplication();

signals:
    void themeChanged(Theme theme);

private:
    ThemeManager();
    void loadOverrides();
    void saveOverrides() const;

    Theme m_theme = Light;
    QHash<QString, QColor> m_lightOverrides;
    QHash<QString, QColor> m_darkOverrides;
};

