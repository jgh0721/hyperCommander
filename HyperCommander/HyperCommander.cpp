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
#include <oclero/qlementine/resources/ResourceInitialization.hpp>
#include <oclero/qlementine/style/QlementineStyle.hpp>
#include <QtitanGrid.h>
#include "solFavoriteDirMgr.hpp"
#include "solTCPluginMgr.hpp"
#include "Modules/mdlFileSystem.hpp"

#include "externalLibs/QtitanDataGrid/src/src/base/QtnCommonStyle.h"

namespace {
// Keep Qtitan's drawing extensions and delegate ordinary controls to Qlementine.
// Both styles have a single owner chain, including across live theme changes.
class ProductStyle final : public Qtitan::CommonStyle {
public:
    ProductStyle() : Qtitan::CommonStyle(new oclero::qlementine::QlementineStyle) {
        qlementine_ = static_cast<oclero::qlementine::QlementineStyle*>(baseStyle());
    }
    void Apply(const TyColorScheme& Scheme) {
        const bool Dark = Scheme.IsDarkMode;
        auto Tokens = (Dark ? oclero::qlementine::Theme::makeDark() : oclero::qlementine::Theme::makeLight()).toJson().object();
        const QJsonObject Colors{{"useSystemFonts", true},
            {"backgroundColorMain1", Scheme.FileList_BGColor.name()},
            {"backgroundColorMain2", Scheme.Dialog_BGColor.name()},
            {"backgroundColorMain3", Dark ? "#293241" : "#E8EDF5"},
            {"backgroundColorWorkspace", Scheme.FileList_BGColor.name()},
            {"backgroundColorTabBar", Scheme.Dialog_BGColor.name()},
            {"secondaryColor", Scheme.Dialog_FGColor.name()},
            {"secondaryAlternativeColor", Dark ? "#AAB5C5" : "#586174"},
            {"primaryColor", Dark ? "#9BBEFF" : "#2457C5"},
            {"primaryColorForeground", Dark ? "#102342" : "#FFFFFF"},
            {"borderColor", Dark ? "#536174" : "#BAC4D2"}};
        for (auto It = Colors.begin(); It != Colors.end(); ++It) Tokens[It.key()] = It.value();
        auto Theme = oclero::qlementine::Theme::fromJsonDoc(QJsonDocument(Tokens));
        if (!Theme) return;
        Theme->fontRegular = Scheme.Dialog_Font;
        Theme->fontBold = Scheme.Dialog_Font; Theme->fontBold.setBold(true);
        const int ControlHeight = QFontMetrics(Scheme.Dialog_Font).height() + 12;
        Theme->controlHeightLarge = qMax(Theme->controlHeightLarge, ControlHeight);
        Theme->controlHeightMedium = qMax(Theme->controlHeightMedium, ControlHeight - 4);
        // Placeholder hints describe an available action, so keep them readable
        // instead of inheriting the base theme's disabled-text contrast.
        for (const auto Group : {QPalette::Active, QPalette::Inactive})
            Theme->palette.setColor(Group, QPalette::PlaceholderText, QColor(Dark ? "#AAB5C5" : "#586174"));
        // setTheme/setPalette can synchronously repaint widgets. Publish all
        // custom drawing colors before those notifications reach the grid.
        header_ = Theme->backgroundColorMain3; text_ = Scheme.Dialog_FGColor;
        hover_ = Scheme.FileList_Cursor; selected_ = Scheme.FileList_Selected;
        border_ = Theme->borderColor;
        QPixmapCache::clear();
        qlementine_->setTheme(*Theme);
        QApplication::setPalette(Theme->palette);
        QApplication::setFont(Scheme.Dialog_Font);
        QApplication::setFont(Scheme.Menu_Font, "QMenu");
        QApplication::setFont(Scheme.Menu_Font, "QMenuBar");
        setProperty("hc.theme", Scheme.Name);
        for (const auto Pair : {qMakePair(Qtitan::GridThemeManager::SortAscendingIcon, false),
                               qMakePair(Qtitan::GridThemeManager::SortDescendingIcon, true)}) {
            QPixmap Pixmap(16, 16); Pixmap.fill(Qt::transparent);
            QPainter Painter(&Pixmap); Painter.setPen(Qt::NoPen); Painter.setBrush(Theme->primaryColor);
            Painter.drawPolygon(Pair.second ? QPolygon{{3, 5}, {13, 5}, {8, 11}} : QPolygon{{3, 11}, {13, 11}, {8, 5}});
            Painter.end(); Qtitan::GridBase::themeManager()->setIcon(Pair.first, QIcon(Pixmap));
        }
        Qtitan::GridBase::themeManager()->updateViews();
    }
    QSize sizeFromContents(ContentsType Type, const QStyleOption* Option, const QSize& Contents, const QWidget* Widget = nullptr) const override {
        auto Size = Qtitan::CommonStyle::sizeFromContents(Type, Option, Contents, Widget);
        if (Type == CT_ComboBox || Type == CT_LineEdit || Type == CT_SpinBox)
            Size.setHeight(qMax(Size.height(), Option->fontMetrics.height() + 12));
        if (Type == CT_ComboBox)
            Size.setWidth(qMax(Size.width(), Contents.width() + qlementine_->theme().spacing * 3 + qlementine_->theme().iconSize.width()));
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
        // Qtitan measures an editor before assigning its text. Qlementine would
        // otherwise return an empty content box, hiding the cell's contents.
        // Persistent controls in a settings table also need the entire empty
        // cell; the delegate has no display text to measure in those cells.
        if (Element == SE_ItemViewItemText && (isGrid(Widget) || (Widget && Widget->property("hc.cellControls").toBool()))) return Option->rect;
        return Qtitan::CommonStyle::subElementRect(Element, Option, Widget);
    }
    void drawControl(ControlElement Element, const QStyleOption* Option, QPainter* Painter, const QWidget* Widget = nullptr) const override {
        const auto* Header = qstyleoption_cast<const QStyleOptionHeader*>(Option);
        if (Header && isGrid(Widget)) {
            if (Element == CE_HeaderSection) {
                Painter->fillRect(Header->rect, Header->state.testFlag(State_Sunken) ? selected_ :
                    Header->state.testFlag(State_MouseOver) ? hover_ : header_);
                Painter->save(); Painter->setPen(border_);
                Painter->drawLine(Header->rect.bottomLeft(), Header->rect.bottomRight());
                Painter->drawLine(Header->rect.topRight(), Header->rect.bottomRight()); Painter->restore(); return;
            }
            if (Element == CE_HeaderLabel) {
                auto Copy = *Header; Copy.palette.setColor(QPalette::ButtonText, text_); Copy.palette.setColor(QPalette::Text, text_);
                Qtitan::CommonStyle::drawControl(Element, &Copy, Painter, Widget); return;
            }
        }
        Qtitan::CommonStyle::drawControl(Element, Option, Painter, Widget);
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
    oclero::qlementine::QlementineStyle* qlementine_ = nullptr;
    QColor header_, text_, hover_, selected_, border_;
};
}

HyperCommanderApp::HyperCommanderApp( int& argc, char** argv )
    : QApplication( argc, argv )
{
    // Anchor the statically linked themes, SVGs, and font resources.
    oclero::qlementine::resources::initializeResources();
    auto* Style = new ProductStyle;
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
