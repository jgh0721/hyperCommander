#include "stdafx.h"
#include "HyperCommander.hpp"

#include "cmnHCUtils.hpp"
#include "dlgMain.hpp"
#include "dlgMultiRename.hpp"

#include "UniqueLibs/commandMgr.hpp"
#include "UniqueLibs/colorSchemeMgr.hpp"

#include "cmnTypeDefs.hpp"
#include "cmnTypeDefs_Name.hpp"
#include "cmnTypeDefs_Opts.hpp"
#include "externalEditorMgr.hpp"
#include "fileSetMgr.hpp"
#include "QtnPlatform.h"
#include <fmstyle/ThemeManager.h>
#include <fmstyle/WatercolorStyle.h>
#include <QtitanGrid.h>
#include "solFavoriteDirMgr.hpp"
#include "solTCPluginMgr.hpp"
#include "Modules/mdlFileSystem.hpp"

#include <QtnCommonStyle.h>

namespace {
// QApplication owns the proxy, which owns the installed WatercolorStyle.
// Live Light/Dark changes update tokens without replacing either style object.
class ProductStyle final : public Qtitan::CommonStyle {
public:
    explicit ProductStyle(QStyle* Base) : Qtitan::CommonStyle(Base) {}
    void Apply(const TyColorScheme& Scheme) {
        using namespace fm::style;
        auto& Manager = ThemeManager::instance();
        {
            ThemeManager::Batch Batch(Manager);
            Manager.clearOverrides();
            const auto VariantValue = Scheme.IsDarkMode ? Variant::Dark : Variant::Light;
            if (Scheme.explicitColors_.contains("dialogText"))
                Manager.setOverride(VariantValue, Token::Fg, Scheme.Dialog_FGColor);
            if (Scheme.explicitColors_.contains("dialogBackground"))
                Manager.setOverride(VariantValue, Token::Win, Scheme.Dialog_BGColor);
            Manager.setScheme(Scheme.IsDarkMode ? ThemeManager::Scheme::Dark : ThemeManager::Scheme::Light);
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
        auto Size = Qtitan::CommonStyle::sizeFromContents(Type, Option, Contents, Widget);
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
        return Qtitan::CommonStyle::subElementRect(Element, Option, Widget);
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
    auto& Theme = fm::style::ThemeManager::instance();
    Theme.setDesign(fm::style::Design::Watercolor);
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
        QTimer::singleShot(0, this, [this, Error] { QMessageBox::warning(mainUI, tr("설정 저장 실패"), Error); });
    });
    initialize();
    const auto Colors = TyStColorSchemeMgr::GetInstance();
    Style->Apply(Colors->GetColorScheme(Colors->GetCurrentColorScheme()));
    connect(Colors, &CColorSchemeMgr::sigColorSchemeChanged, this, [Style](const TyColorScheme& Scheme) { Style->Apply(Scheme); });
    if (!Settings.Exists() && Settings.Writable()) Settings.Commit();

    mainUI = new QMainUI;
    mainUI->show();
    if (!Settings.Error().isEmpty() || !Settings.Issues().isEmpty())
        mainUI->statusBar()->showMessage(tr("설정 확인 필요: %1 %2").arg(Settings.Error(), Settings.Issues().join(", ")));
}

void HyperCommanderApp::ShowMultiRename( const QVector< QString >& VecFiles )
{
    QMultiRenameUI MultiRename;
    MultiRename.SetSourceFiles( VecFiles );
    MultiRename.exec();
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
