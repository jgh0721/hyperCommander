#include "core/ToolbarIcons.h"

#include "core/ThemeManager.h"
#include <fmstyle/ThemeManager.h>

#include <QDebug>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QSet>
#include <QSvgRenderer>
#include <QToolBar>

namespace {

constexpr auto kActionKeyProperty = "mv.toolbarActionKey";
constexpr auto kTextBehaviorProperty = "mv.toolbarActionTextBehavior";
constexpr auto kManifestResourcePath = ":/icons/toolbar/manifest.json";

QString manifestResourcePath()
{
    return QString::fromLatin1(kManifestResourcePath);
}

struct IconPalette
{
    QColor normalOff;
    QColor activeOff;
    QColor disabledOff;
    QColor normalOn;
    QColor activeOn;
    QColor disabledOn;
};

const QHash<QString, ToolbarIcons::ActionMeta>& actionRegistry()
{
    static const QHash<QString, ToolbarIcons::ActionMeta> registry = [] {
        QHash<QString, ToolbarIcons::ActionMeta> loaded;
        QFile file(manifestResourcePath());
        if (!file.open(QIODevice::ReadOnly)) {
            qWarning().noquote() << QStringLiteral("[ToolbarIcons] Failed to open manifest '%1'.").arg(manifestResourcePath());
            return loaded;
        }

        const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
        if (!document.isObject()) {
            qWarning().noquote() << QStringLiteral("[ToolbarIcons] Manifest '%1' is not a valid JSON object.").arg(manifestResourcePath());
            return loaded;
        }

        const QJsonArray actions = document.object().value(QStringLiteral("actions")).toArray();
        for (const QJsonValue& value : actions) {
            const QJsonObject object = value.toObject();
            const QString key = object.value(QStringLiteral("key")).toString();
            if (key.isEmpty())
                continue;

            loaded.insert(key, ToolbarIcons::ActionMeta{
                key,
                object.value(QStringLiteral("iconName")).toString(),
                object.value(QStringLiteral("toolTip")).toString(),
                object.value(QStringLiteral("checkable")).toBool(false)
            });
        }

        return loaded;
    }();
    return registry;
}

ToolbarIcons::ActionMeta missingMeta()
{
    return {QStringLiteral("__missing__"), QStringLiteral("missing-icon"), QObject::tr("등록되지 않은 툴바 액션"), false};
}

QString resourcePathForIcon(const QString& iconName)
{
    return QStringLiteral(":/icons/toolbar/%1.svg").arg(iconName);
}

void warnMissingMeta(const QString& actionKey, const char* file, int line)
{
    qWarning().noquote() << QStringLiteral("[ToolbarIcons] Unregistered action key '%1' at %2:%3; using missing-icon.")
                            .arg(actionKey, QString::fromUtf8(file), QString::number(line));
}

void warnMissingSvg(const QString& iconName)
{
    qWarning().noquote() << QStringLiteral("[ToolbarIcons] Missing SVG resource for icon '%1'; using missing-icon.").arg(iconName);
}

void warnInvalidSvg(const QString& iconName)
{
    static QSet<QString> warned;
    if (warned.contains(iconName))
        return;

    warned.insert(iconName);
    qWarning().noquote() << QStringLiteral("[ToolbarIcons] Invalid SVG payload for icon '%1'; using painted fallback.").arg(iconName);
}

const ToolbarIcons::ActionMeta& resolveMeta(const QString& actionKey, const char* file, int line)
{
    const auto it = actionRegistry().find(actionKey);
    if (it != actionRegistry().end())
        return it.value();

    static const ToolbarIcons::ActionMeta fallback = missingMeta();
    warnMissingMeta(actionKey, file, line);
    return fallback;
}

QByteArray loadSvgTemplate(const QString& iconName)
{
    static QHash<QString, QByteArray> cache;

    // Empty icon name → use fallback directly (no recursion)
    const QString effectiveName = iconName.isEmpty() ? QStringLiteral("missing-icon") : iconName;

    if (cache.contains(effectiveName))
        return cache.value(effectiveName);

    QFile file(resourcePathForIcon(effectiveName));
    if (!file.open(QIODevice::ReadOnly)) {
        if (effectiveName != QStringLiteral("missing-icon"))
            warnMissingSvg(effectiveName);
        QFile fallback(resourcePathForIcon(QStringLiteral("missing-icon")));
        if (fallback.open(QIODevice::ReadOnly))
            cache.insert(effectiveName, fallback.readAll());
        else
            cache.insert(effectiveName, QByteArray());
        return cache.value(effectiveName);
    }

    cache.insert(effectiveName, file.readAll());
    return cache.value(effectiveName);
}

void paintFallbackGlyph(QPainter& painter, const QSize& size, const QColor& color)
{
    const QRectF bounds(QPointF(0.0, 0.0), QSizeF(size));
    QPen pen(color);
    pen.setWidthF(qMax(1.4, size.width() / 10.0));
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    const QRectF frame = bounds.adjusted(size.width() * 0.18,
                                         size.height() * 0.18,
                                         -size.width() * 0.18,
                                         -size.height() * 0.18);
    painter.drawRoundedRect(frame, size.width() * 0.08, size.height() * 0.08);
    painter.drawLine(frame.topLeft(), frame.bottomRight());
}

IconPalette paletteForTheme(ThemeManager::Theme theme)
{
    // Product tokens of the requested variant: muted glyphs, full text color on hover, accent when checked.
    using fm::style::Token;
    const auto& Manager = fm::style::ThemeManager::instance();
    const bool Dark = theme == ThemeManager::Dark;
    const auto& T = fm::style::isDarkVariant(Manager.effectiveVariant()) == Dark
        ? Manager.colors() : Manager.colors(Dark ? fm::style::Variant::Dark : fm::style::Variant::Light);
    QColor DisabledOn = T[Token::Accent]; DisabledOn.setAlphaF(0.45f);
    return { T[Token::Fg2], T[Token::Fg], T[Token::Fg3], T[Token::Accent], T[Token::AccentFg], DisabledOn };
}

QColor colorFor(ThemeManager::Theme theme, QIcon::Mode mode, QIcon::State state)
{
    const IconPalette palette = paletteForTheme(theme);

    switch (mode) {
        case QIcon::Active:
            return state == QIcon::On ? palette.activeOn : palette.activeOff;
        case QIcon::Disabled:
            return state == QIcon::On ? palette.disabledOn : palette.disabledOff;
        case QIcon::Normal:
        default:
            return state == QIcon::On ? palette.normalOn : palette.normalOff;
    }
}

QPixmap renderTintedIcon(const QString& iconName, const QByteArray& svgTemplate, const QColor& color, const QSize& size)
{
    if (!size.isValid() || size.isEmpty())
        return {};

    const qreal dpr = 2.0;
    const QSize pixelSize(qRound(size.width() * dpr), qRound(size.height() * dpr));
    QImage image(pixelSize, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull())
        return {};

    image.fill(Qt::transparent);

    QPainter painter(&image);
    if (!painter.isActive())
        return {};

    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.scale(dpr, dpr);

    bool rendered = false;
    if (!svgTemplate.isEmpty() && svgTemplate.size() > 10) {
        QSvgRenderer renderer;
        if (renderer.load(svgTemplate) && renderer.isValid()) {
            renderer.render(&painter, QRectF(QPointF(0.0, 0.0), QSizeF(size)));
            painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
            painter.fillRect(QRectF(QPointF(0.0, 0.0), QSizeF(size)), color);
            rendered = true;
        } else {
            warnInvalidSvg(iconName);
        }
    }

    if (!rendered)
        paintFallbackGlyph(painter, size, color);

    painter.end();

    QPixmap pixmap = QPixmap::fromImage(image);
    pixmap.setDevicePixelRatio(dpr);
    return pixmap;
}

QIcon buildIcon(const QString& iconName)
{
    const QString effectiveName = iconName.isEmpty() ? QStringLiteral("missing-icon") : iconName;
    static QHash<QString, QIcon> cache;
    const auto CurrentTheme = ThemeManager::instance().currentTheme();
    const QString cacheKey = QStringLiteral("%1:%2:%3:%4")
        .arg(static_cast<int>(CurrentTheme))
        .arg(colorFor(CurrentTheme, QIcon::Normal, QIcon::Off).name(QColor::HexArgb), colorFor(CurrentTheme, QIcon::Normal, QIcon::On).name(QColor::HexArgb))
        .arg(effectiveName);
    if (cache.contains(cacheKey))
        return cache.value(cacheKey);

    const QByteArray svgTemplate = loadSvgTemplate(effectiveName);
    QIcon icon;
    const auto theme = ThemeManager::instance().currentTheme();
    const QList<QSize> sizes{QSize(16, 16), QSize(20, 20), QSize(24, 24), QSize(32, 32)};
    const QList<QIcon::Mode> modes{QIcon::Normal, QIcon::Active, QIcon::Disabled};
    const QList<QIcon::State> states{QIcon::Off, QIcon::On};

    for (QIcon::Mode mode : modes) {
        for (QIcon::State state : states) {
            const QColor color = colorFor(theme, mode, state);
            for (const QSize& size : sizes) {
                QPixmap pm = renderTintedIcon(effectiveName, svgTemplate, color, size);
                if (!pm.isNull())
                    icon.addPixmap(pm, mode, state);
            }
        }
    }

    cache.insert(cacheKey, icon);
    return cache.value(cacheKey);
}

} // namespace

namespace ToolbarIcons {

QAction* createAction(QToolBar* toolBar, const QString& actionKey, const char* file, int line)
{
    if (!toolBar)
        return nullptr;

    auto* action = new QAction(toolBar);
    apply(action, actionKey, file, line);
    toolBar->addAction(action);
    return action;
}

void apply(QAction* action, const QString& actionKey, const char* file, int line, TextBehavior textBehavior)
{
    if (!action)
        return;

    const ActionMeta& meta = resolveMeta(actionKey, file, line);
    action->setProperty(kActionKeyProperty, meta.key);
    action->setProperty(kTextBehaviorProperty, static_cast<int>(textBehavior));
    action->setObjectName(meta.key);
    action->setCheckable(meta.checkable);
    if (textBehavior == TextBehavior::ClearText)
        action->setText(QString());
    else if (action->text().isEmpty())
        action->setText(meta.toolTip);
    action->setIconText(meta.toolTip);
    action->setToolTip(meta.toolTip);
    action->setStatusTip(meta.toolTip);
    action->setWhatsThis(meta.toolTip);
    action->setIcon(buildIcon(meta.iconName));
}

void refreshAction(QAction* action)
{
    if (!action)
        return;

    const QString actionKey = action->property(kActionKeyProperty).toString();
    if (actionKey.isEmpty())
        return;

    const QVariant textBehaviorValue = action->property(kTextBehaviorProperty);
    const auto textBehavior = static_cast<TextBehavior>(
        textBehaviorValue.isValid()
            ? textBehaviorValue.toInt()
            : static_cast<int>(TextBehavior::ClearText));
    apply(action, actionKey, __FILE__, __LINE__, textBehavior);
}

ActionMeta metaForActionKey(const QString& actionKey, const char* file, int line)
{
    return resolveMeta(actionKey, file, line);
}

QIcon iconForActionKey(const QString& actionKey, const char* file, int line)
{
    return buildIcon(resolveMeta(actionKey, file, line).iconName);
}

QIcon iconByName(const QString& iconName)
{
    return buildIcon(iconName);
}

void refreshToolBar(QToolBar* toolBar)
{
    if (!toolBar)
        return;

    const auto actions = toolBar->actions();
    for (QAction* action : actions) {
        refreshAction(action);
    }
}

QStringList registeredActionKeys()
{
    QStringList keys = actionRegistry().keys();
    keys.sort();
    return keys;
}

} // namespace ToolbarIcons


