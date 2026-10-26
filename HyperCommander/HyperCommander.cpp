#include "stdafx.h"
#include "UniqueLibs/settings/singleInstance.hpp"
#include "UniqueLibs/qtMessageDialog.hpp"
#include <QStandardPaths>
#include "HyperCommander.hpp"
#include "UniqueLibs/startupTrace.hpp"
#include "UIs/Components/cmpView.hpp"

#include "cmnHCUtils.hpp"
#include "dlgMain.hpp"

#include "UniqueLibs/commandMgr.hpp"
#include "UniqueLibs/colorSchemeMgr.hpp"

#include "cmnTypeDefs.hpp"
#include "cmnTypeDefs_Name.hpp"
#include "cmnTypeDefs_Opts.hpp"
#include "externalEditorMgr.hpp"
#include "fileSetMgr.hpp"
#include "QtnPlatform.h"
#include <fmstyle/ThemeManager.h>
#include "UniqueLibs/productTheme.hpp"
#include "UniqueLibs/qtTranslation.hpp"
#include <QtitanGrid.h>
#include "solFavoriteDirMgr.hpp"
#include "solTCPluginMgr.hpp"
#include "Modules/mdlFileSystem.hpp"

#include <QtnCommonStyle.h>

namespace {
// QApplication owns the proxy, which owns the installed product style (PaperFluentStyle).
// Live Light/Dark changes update tokens without replacing either style object.
class ProductStyle final : public Qtitan::CommonStyle {
public:
    explicit ProductStyle(QStyle* Base) : Qtitan::CommonStyle(Base) {}
    // CommonStyle paints some standard controls itself. Preserve its Qtitan
    // extensions while routing ordinary Qt controls through the product style.
    void drawPrimitive(PrimitiveElement Element, const QStyleOption* Option, QPainter* Painter, const QWidget* Widget = nullptr) const override {
        // Selection already identifies the current item; omit the dotted focus
        // frame while preserving keyboard focus and navigation.
        if (Element == PE_FrameFocusRect) return;
        if (Element >= PE_CustomBase) Qtitan::CommonStyle::drawPrimitive(Element, Option, Painter, Widget);
        else QProxyStyle::drawPrimitive(Element, Option, Painter, Widget);
    }
    void drawControl(ControlElement Element, const QStyleOption* Option, QPainter* Painter, const QWidget* Widget = nullptr) const override {
        if (Element >= CE_CustomBase) Qtitan::CommonStyle::drawControl(Element, Option, Painter, Widget);
        else QProxyStyle::drawControl(Element, Option, Painter, Widget);
    }
    void drawComplexControl(ComplexControl Control, const QStyleOptionComplex* Option, QPainter* Painter, const QWidget* Widget = nullptr) const override {
        if (Control >= CC_CustomBase) Qtitan::CommonStyle::drawComplexControl(Control, Option, Painter, Widget);
        else QProxyStyle::drawComplexControl(Control, Option, Painter, Widget);
    }
    QPalette standardPalette() const override { return baseStyle()->standardPalette(); }
    void polish(QPalette& Palette) override { baseStyle()->polish(Palette); }
    void polish(QApplication* App) override {
        Qtitan::CommonStyle::polish(App);
        App->setPalette(fm::style::ThemeManager::instance().colors().toPalette());
    }
    int pixelMetric(PixelMetric Metric, const QStyleOption* Option = nullptr, const QWidget* Widget = nullptr) const override {
        return Metric >= PM_CustomBase ? Qtitan::CommonStyle::pixelMetric(Metric, Option, Widget) : QProxyStyle::pixelMetric(Metric, Option, Widget);
    }
    QRect subControlRect(ComplexControl Control, const QStyleOptionComplex* Option, SubControl Sub, const QWidget* Widget = nullptr) const override {
        return Control >= CC_CustomBase ? Qtitan::CommonStyle::subControlRect(Control, Option, Sub, Widget) : QProxyStyle::subControlRect(Control, Option, Sub, Widget);
    }
    void Apply(const TyColorScheme& Scheme) {
        using namespace fm::style;
        auto& Manager = ThemeManager::instance();
        {
            ThemeManager::Batch Batch(Manager);
            Manager.clearOverrides();
            const auto General = hc::settings::AppSettings().Read(hc::settings::Section::General).toObject();
            const auto Appearance = hc::settings::AppSettings().Read(hc::settings::Section::Appearance).toObject();
            Manager.setColorScheme(hc::theme::ReadProductScheme(Appearance["watercolorTheme"].toObject()));
            Manager.setDarkTitleBar(General["darkTitleBar"].toBool(true));
            Manager.setMonoFont(General["monoFontFamily"].toString("Cascadia Mono"), General["monoFontPx"].toInt(12));
            const auto VariantValue = Scheme.IsDarkMode ? Variant::Dark : Variant::Light;
            if (Scheme.explicitColors_.contains("dialogText"))
                Manager.setOverride(VariantValue, Token::Fg, Scheme.Dialog_FGColor);
            if (Scheme.explicitColors_.contains("dialogBackground"))
                Manager.setOverride(VariantValue, Token::Win, Scheme.Dialog_BGColor);
            Manager.setScheme(General["themeMode"] == "system" ? ThemeManager::Scheme::System : Scheme.IsDarkMode ? ThemeManager::Scheme::Dark : ThemeManager::Scheme::Light);
            QPixmapCache::clear();
        }
        QApplication::setFont(Scheme.Dialog_Font);
        QApplication::setFont(Scheme.Menu_Font, "QMenu");
        QApplication::setFont(Scheme.Menu_Font, "QMenuBar");
        setProperty("hc.theme", Scheme.Name);
        const auto Accent = Manager.colors()[Token::Accent];
        for (const auto Pair : {qMakePair(Qtitan::GridThemeManager::SortAscendingIcon, false),
                               qMakePair(Qtitan::GridThemeManager::SortDescendingIcon, true)}) {
            QPixmap Pixmap(16, 16); Pixmap.fill(Qt::transparent);
            QPainter Painter(&Pixmap); Painter.setPen(Qt::NoPen); Painter.setBrush(Accent);
            Painter.drawPolygon(Pair.second ? QPolygon{{3, 5}, {13, 5}, {8, 11}} : QPolygon{{3, 11}, {13, 11}, {8, 5}});
            Painter.end(); Qtitan::GridBase::themeManager()->setIcon(Pair.first, QIcon(Pixmap));
        }
        // Grid consumers apply their palette and file colors later in the same
        // signal delivery. Avoid painting an intermediate palette synchronously.
        if (!refreshPending_) {
            refreshPending_ = true;
            QTimer::singleShot(0, this, [this] {
                refreshPending_ = false;
                Qtitan::GridBase::themeManager()->updateViews();
            });
        }
    }
    QSize sizeFromContents(ContentsType Type, const QStyleOption* Option, const QSize& Contents, const QWidget* Widget = nullptr) const override {
        auto Size = Type >= CT_CustomBase ? Qtitan::CommonStyle::sizeFromContents(Type, Option, Contents, Widget) : QProxyStyle::sizeFromContents(Type, Option, Contents, Widget);
        if (Type == CT_ComboBox || Type == CT_LineEdit || Type == CT_SpinBox)
            Size.setHeight(qMax(Size.height(), Option->fontMetrics.height() + 12));
        if (Type == CT_ComboBox)
            Size.setWidth(qMax(Size.width(), Contents.width() + 32));
        return Size;
    }
    using Qtitan::CommonStyle::polish;
    using Qtitan::CommonStyle::unpolish;
    void polish(QWidget* Widget) override {
        Qtitan::CommonStyle::polish(Widget);
        if (qobject_cast<QComboBox*>(Widget)) {
            if (!Widget->property("hc.originalMinimumHeight").isValid())
                Widget->setProperty("hc.originalMinimumHeight", Widget->minimumHeight());
            Widget->installEventFilter(this);
            fitComboHeight(Widget);
        }
    }
    void unpolish(QWidget* Widget) override {
        Widget->removeEventFilter(this);
        Qtitan::CommonStyle::unpolish(Widget);
    }
    bool eventFilter(QObject* Object, QEvent* Event) override {
        if (Event->type() == QEvent::FontChange || Event->type() == QEvent::ApplicationFontChange)
            if (auto* Widget = qobject_cast<QComboBox*>(Object)) fitComboHeight(Widget);
        return Qtitan::CommonStyle::eventFilter(Object, Event);
    }
    QRect subElementRect(SubElement Element, const QStyleOption* Option, const QWidget* Widget = nullptr) const override {
        // Qtitan measures an editor before assigning its text. Keep the complete
        // content box for editors and persistent controls in empty cells.
        // Persistent controls in a settings table also need the entire empty
        // cell; the delegate has no display text to measure in those cells.
        if (Element == SE_ItemViewItemText && (isGrid(Widget) || (Widget && Widget->property("hc.cellControls").toBool()))) return Option->rect;
        return Element >= SE_CustomBase ? Qtitan::CommonStyle::subElementRect(Element, Option, Widget) : QProxyStyle::subElementRect(Element, Option, Widget);
    }
private:
    static void fitComboHeight(QWidget* Widget) {
        Widget->setMinimumHeight(qMax(Widget->property("hc.originalMinimumHeight").toInt(), Widget->fontMetrics().height() + 12));
        Widget->updateGeometry();
    }
    static bool isGrid(const QWidget* Widget) {
        for (auto* Current = Widget; Current; Current = Current->parentWidget())
            if (Current->property("hc.fileGrid").toBool()) return true;
        return false;
    }
    bool refreshPending_ = false;
};
}

HyperCommanderApp::HyperCommanderApp( int& argc, char** argv )
    : QApplication( argc, argv )
{
    hc::ui::InstallQtTranslation(*this);
    auto& Theme = fm::style::ThemeManager::instance();
    Theme.setDesign(hc::theme::ProductDesign);
    Theme.setScheme(fm::style::ThemeManager::Scheme::Light);
    Theme.install(*this);
    auto* Style = new ProductStyle(QApplication::style());
    QApplication::setStyle(Style);

    auto Font = QApplication::font();
    Font.setStyleStrategy( QFont::PreferAntialias  );
    Font.setHintingPreference( QFont::PreferFullHinting );
    QApplication::setFont( Font );
    
    QApplication::setWindowIcon( QIcon( ":/HyperCommander/Logo" ) );
    QPixmapCache::setCacheLimit( 65535 );

    QCoreApplication::setOrganizationName( "MyHouse" );
    QCoreApplication::setOrganizationDomain( "devtester.com" );
    QCoreApplication::setApplicationName( "HyperCommander" );

    SetEnvironmentVariableW( L"COMMANDER_PATH", QDir::toNativeSeparators( applicationDirPath() ).toStdWString().c_str() );

    const auto Config = hc::settings::Paths();
    if (!Config.error_.isEmpty()) qFatal("Invalid settings path: %s", qPrintable(Config.error_));
    auto& Settings = hc::settings::AppSettings();
    Settings.SetErrorHandler([this](const QString& Error) {
        if (hc::settings::AppSettings().Editing()) return;
        QTimer::singleShot(0, this, [this, Error] { hc::ui::MessageDialog::warning(mainUI, tr("설정 저장 실패"), Error); });
    });
    const auto StartupPaths = hc::settings::SingleInstance::Paths(QCoreApplication::arguments());
    if (!QStandardPaths::isTestModeEnabled() && Settings.Read(hc::settings::Section::General).toObject()["singleInstance"].toBool(false)) {
        singleInstance_ = new hc::settings::SingleInstance(this);
        const auto Result = singleInstance_->Start(Config.directory_, StartupPaths, [this](const QJsonArray& Paths) {
            return mainUI && mainUI->OpenStartupPaths(Paths);
        });
        if (Result != hc::settings::SingleInstance::Result::Primary) {
            shouldRun_ = false;
            if (Result == hc::settings::SingleInstance::Result::Failed) {
                startupExitCode_ = 1;
                hc::ui::MessageDialog::warning(nullptr, tr("기존 창 연결 실패"), singleInstance_->Error());
            }
            return;
        }
    }
    hc::startup::Record("app.initialize.begin");
    initialize();
    hc::startup::Record("app.initialize.end");
    const auto Colors = TyStColorSchemeMgr::GetInstance();
    Style->Apply(Colors->GetColorScheme(Colors->GetCurrentColorScheme()));
    connect(Colors, &CColorSchemeMgr::sigColorSchemeChanged, this, [Style](const TyColorScheme& Scheme) { Style->Apply(Scheme); });
    connect(styleHints(), &QStyleHints::colorSchemeChanged, this, [](Qt::ColorScheme) {
        auto& Settings = hc::settings::AppSettings();
        if (Settings.Editing() || Settings.Read(hc::settings::Section::General).toObject()["themeMode"] != "system") return;
        auto* Colors = TyStColorSchemeMgr::GetInstance(); Colors->Refresh(); emit Colors->sigColorSchemeChanged(Colors->GetColorScheme(Colors->GetCurrentColorScheme()));
    });
    if (!Settings.Exists() && Settings.Writable()) Settings.Commit();

    mainUI = new QMainUI;
    mainUI->show();
    hc::startup::Record("window.show");
    if(hc::startup::Enabled()){auto* Timer=new QTimer(this);Timer->setSingleShot(true);connect(Timer,&QTimer::timeout,this,[]{hc::startup::Flush();});Timer->start(20000);}
    if (!QStandardPaths::isTestModeEnabled() && !StartupPaths.isEmpty())
        QTimer::singleShot(0, mainUI, [this, StartupPaths] { mainUI->OpenStartupPaths(StartupPaths); });
    if (!Settings.Error().isEmpty() || !Settings.Issues().isEmpty())
        mainUI->statusBar()->showMessage(tr("설정 확인 필요: %1 %2").arg(Settings.Error(), Settings.Issues().join(", ")));
}

///////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////

void HyperCommanderApp::initialize()
{
    const auto StCommandMgr = TyStCommandMgr::GetInstance();
    const auto StColorSchemeMgr = TyStColorSchemeMgr::GetInstance();

    TyStExternalEditorMgr::GetInstance()->Refresh();
    TyStPlugInMgr::GetInstance()->Refresh();
    TyStFileSetMgr::GetInstance()->Refresh();
    TyStFavoriteDirMgr::GetInstance()->Refresh();

    StCommandMgr->Refresh();
    StColorSchemeMgr->Refresh();
}

bool HyperCommanderApp::notify(QObject* Receiver,QEvent* Event)
{
    if(!hc::startup::Enabled()||Event->type()!=QEvent::Paint)return QApplication::notify(Receiver,Event);
    QPointer<CGridView> View;
    for(auto* Object=Receiver;Object;Object=Object->parent())if((View=qobject_cast<CGridView*>(Object)))break;
    const bool Result=QApplication::notify(Receiver,Event);
    // Qt paint delivery has returned. This is not a DWM presentation fence.
    if(View)View->StartupPaintCompleted();return Result;
}
