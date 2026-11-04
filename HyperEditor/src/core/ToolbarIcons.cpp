#include "stdafx.h"
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
#include <QIconEngine>
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

QPixmap renderTintedIcon(const QString& iconName, const QByteArray& svgTemplate, const QColor& color, const QSize& size, qreal dpr = 2.0)
{
    if (!size.isValid() || size.isEmpty())
        return {};

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

// Defer rasterization until Qt requests an actual size/state. The previous path
// rendered 24 pixmaps per icon, including icons in menus never opened.
class LazyToolbarIcon final : public QIconEngine
{
public:
    explicit LazyToolbarIcon(QString Name) : name_(std::move(Name)), svg_(loadSvgTemplate(name_))
    {
        const auto Theme = ThemeManager::instance().currentTheme();
        for (int Mode = 0; Mode < 4; ++Mode)
            for (int State = 0; State < 2; ++State)
                colors_[Mode][State] = colorFor(Theme, QIcon::Mode(Mode), QIcon::State(State));
    }
    QIconEngine* clone() const override { return new LazyToolbarIcon(*this); }
    bool isNull() override { return false; }
    QPixmap pixmap(const QSize& Size, QIcon::Mode Mode, QIcon::State State) override
    {
        return scaledPixmap(Size, Mode, State, 1.0);
    }
    QPixmap scaledPixmap(const QSize& Size, QIcon::Mode Mode, QIcon::State State, qreal Scale) override
    {
        const auto Key = QString("%1:%2:%3:%4:%5").arg(Size.width()).arg(Size.height()).arg(int(Mode)).arg(int(State)).arg(Scale);
        const auto Found = cache_.constFind(Key);
        if (Found != cache_.cend()) return Found.value();
        auto Pixmap = renderTintedIcon(name_, svg_, colors_[int(Mode)][int(State)], Size, Scale);
        if (cache_.size() >= 64) cache_.clear();
        cache_.insert(Key, Pixmap);
        return Pixmap;
    }
    void paint(QPainter* Painter, const QRect& Rect, QIcon::Mode Mode, QIcon::State State) override
    {
        const auto Pixmap = scaledPixmap(Rect.size(), Mode, State, Painter->device()->devicePixelRatioF());
        Painter->drawPixmap(Rect, Pixmap);
    }
private:
    QString name_;
    QByteArray svg_;
    QColor colors_[4][2];
    QHash<QString, QPixmap> cache_;
};

QIcon buildIcon(const QString& iconName)
{
    const QString effectiveName = iconName.isEmpty() ? QStringLiteral("missing-icon") : iconName;
    static QHash<QString, QIcon> cache;
    const auto CurrentTheme = ThemeManager::instance().currentTheme();
    QString cacheKey = effectiveName;
    for (int Mode = 0; Mode < 4; ++Mode)
        for (int State = 0; State < 2; ++State)
            cacheKey += ':' + colorFor(CurrentTheme, QIcon::Mode(Mode), QIcon::State(State)).name(QColor::HexArgb);
    if (const auto Found = cache.constFind(cacheKey); Found != cache.cend()) return Found.value();
    if (cache.size() >= 512) cache.clear();
    const QIcon Icon(new LazyToolbarIcon(effectiveName));
    cache.insert(cacheKey, Icon);
    return Icon;
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


