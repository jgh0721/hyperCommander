#include "stdafx.h"
#include "colorSchemeMgr.hpp"

#include <private/qcssparser_p.h>
#include <fmstyle/ThemeColors.h>
#include <fmstyle/ThemeManager.h>
#include <QStyleHints>

#include "cmnTypeDefs_Opts.hpp"
#include "Modules/mdlFileSystem.hpp"

DECLARE_CMNLIBSV2_NAMESPACE

namespace {
struct ColorBinding {
    const char* key_;
    QColor TyColorScheme::*field_;
    fm::style::Token token_;
};
const ColorBinding ColorBindings[] = {
    {"dialogText", &TyColorScheme::Dialog_FGColor, fm::style::Token::Fg},
    {"dialogBackground", &TyColorScheme::Dialog_BGColor, fm::style::Token::Win},
    {"listText", &TyColorScheme::FileList_FGColor, fm::style::Token::Fg},
    {"listBackground", &TyColorScheme::FileList_BGColor, fm::style::Token::Surface},
    {"listCursor", &TyColorScheme::FileList_Cursor, fm::style::Token::AccentSoft},
    // HC marks retain per-file foregrounds and invert by swapping the two colors.
    // Use the neutral selection token instead of the white-on-accent fill.
    {"listSelection", &TyColorScheme::FileList_Selected, fm::style::Token::SelIn}
};
fm::style::ThemeColors defaultColors(bool Dark) {
    using namespace fm::style;
    const auto Object = hc::settings::AppSettings().Read(hc::settings::Section::Appearance).toObject()["watercolorTheme"].toObject();
    auto Scheme = ColorScheme::fromJson(Object).value_or(ColorScheme{});
    if (Scheme.seeds.useSystemAccent) Scheme.seeds.accent = ThemeManager::systemAccent();
    const auto V = Dark ? Variant::Dark : Variant::Light;
    return deriveTheme(V, Scheme.seeds, Scheme.overridesFor(Design::Watercolor, V), Design::Watercolor).colors;
}
}

////////////////////////////////////////////////////////////////////////////////
///

void CColorSchemeMgr::Refresh()
{
    mapNameToScheme.clear(); currentColorScheme = {};
    const auto Appearance = hc::settings::AppSettings().Read(hc::settings::Section::Appearance).toObject();
    const auto Schemes = Appearance["schemes"].toObject();
    for (auto It = Schemes.begin(); It != Schemes.end(); ++It) {
        auto Record = It.value().toObject();
        const auto Fallback = hc::settings::BuiltInAppearance()["schemes"].toObject().value(
            Record["dark"].toBool() ? QStringLiteral("기본 다크") : QStringLiteral("기본 라이트")).toObject();
        QStringList Issues; Record = hc::settings::MergeDefaults(Record, Fallback, {}, Issues);
        TyColorScheme Scheme; Scheme.Name = It.key(); Scheme.IsDarkMode = Record["dark"].toBool();
        if (It.key() == Appearance["selected"].toString()) {
            // The selected product mode is authoritative. Older saved schemes
            // can contain a dark palette with dark=false; mixing those colors
            // with light control surfaces makes text unreadable.
            const auto Mode = hc::settings::AppSettings().Read(hc::settings::Section::General).toObject()["themeMode"].toString();
            if (Mode == "system") Scheme.IsDarkMode = qApp->styleHints()->colorScheme() == Qt::ColorScheme::Dark;
            else if (Mode == "dark" || Mode == "light") Scheme.IsDarkMode = Mode == "dark";
        }
        Scheme.IsInverseCursor = Record["inverseCursor"].toBool();
        Scheme.IsInverseSelect = Record["inverseSelection"].toBool();
        const auto Fonts = Record["fonts"].toObject(); const auto Colors = Record["colors"].toObject();
        Scheme.Menu_Font = retrieveFont(Fonts["menu"].toObject(), 9);
        Scheme.Dialog_Font = retrieveFont(Fonts["dialog"].toObject(), 9);
        Scheme.FileList_Font = retrieveFont(Fonts["list"].toObject(), 14);
        const auto Defaults = defaultColors(Scheme.IsDarkMode);
        const auto Legacy = Fallback["colors"].toObject();
        const auto Explicit = Record.value("colorOverrides").toObject();
        for (const auto& Binding : ColorBindings) {
            const auto Value = QColor::fromString(Colors[Binding.key_].toString());
            // Old files contain fully materialized defaults but no provenance.
            // Equal legacy defaults follow the new theme; explicit metadata wins.
            const bool Override = Record.value("colorOverrides").isObject()
                ? Explicit[Binding.key_].toBool()
                : Value.isValid() && Value != QColor::fromString(Legacy[Binding.key_].toString());
            Scheme.*Binding.field_ = Override ? Value : Defaults[Binding.token_];
            if (Override) Scheme.explicitColors_.insert(Binding.key_);
        }
        const auto FileSets = Record["fileSets"].toObject();
        for (auto File = FileSets.begin(); File != FileSets.end(); ++File) {
            const auto Pair = File.value().toObject();
            Scheme.MapNameToColors[File.key()] = {QColor::fromString(Pair["foreground"].toString()), QColor::fromString(Pair["background"].toString())};
        }
        mapNameToScheme[Scheme.Name] = Scheme;
    }
    const auto Selected = Appearance["selected"].toString();
    if (mapNameToScheme.contains(Selected)) currentColorScheme = mapNameToScheme[Selected];
    else currentColorScheme = mapNameToScheme.value(hc::settings::BuiltInAppearance()["selected"].toString());
}

QVector<QString> CColorSchemeMgr::GetNames() const
{
    return mapNameToScheme.keys();
}

TyColorScheme CColorSchemeMgr::GetColorScheme( const QString& Name ) const
{
    if( Name.isEmpty() == true || mapNameToScheme.contains( Name ) == false )
        return {};

    return mapNameToScheme[ Name ];
}

QString CColorSchemeMgr::GetCurrentColorScheme() const
{
    return currentColorScheme.Name;
}

void CColorSchemeMgr::SetCurrentColorScheme( const QString& Name )
{
    if( mapNameToScheme.contains( Name ) == false )
        return;

    currentColorScheme = mapNameToScheme[ Name ];
}

void CColorSchemeMgr::UpsertColorScheme( const TyColorScheme& ColorScheme, bool IsWriteToFile )
{
    if (ColorScheme.Name.trimmed().isEmpty()) return;
    auto Resolved = ColorScheme;
    const auto Previous = mapNameToScheme.value(ColorScheme.Name);
    const auto Defaults = defaultColors(ColorScheme.IsDarkMode);
    for (const auto& Binding : ColorBindings) {
        const auto Value = ColorScheme.*Binding.field_;
        const auto Old = Previous.Name.isEmpty() ? Defaults[Binding.token_] : Previous.*Binding.field_;
        if (Previous.explicitColors_.contains(Binding.key_) || Value != Old)
            Resolved.explicitColors_.insert(Binding.key_);
        if (!Resolved.explicitColors_.contains(Binding.key_))
            Resolved.*Binding.field_ = Defaults[Binding.token_];
    }
    mapNameToScheme[ColorScheme.Name] = Resolved;
    if (IsWriteToFile) {
        auto Appearance = hc::settings::AppSettings().Read(hc::settings::Section::Appearance).toObject();
        auto Schemes = Appearance["schemes"].toObject();
        const auto EncodeFont = [](const QFont& Font) { return QJsonObject{{"family",Font.family()},{"pointSize",Font.pointSize() > 0 ? Font.pointSize() : 9}}; };
        for (const auto& Scheme : mapNameToScheme) {
            auto Record = Schemes[Scheme.Name].toObject();
            Record["dark"] = Scheme.IsDarkMode;
            Record["inverseCursor"] = Scheme.IsInverseCursor;
            Record["inverseSelection"] = Scheme.IsInverseSelect;
            Record["fonts"] = QJsonObject{{"menu",EncodeFont(Scheme.Menu_Font)},{"dialog",EncodeFont(Scheme.Dialog_Font)},{"list",EncodeFont(Scheme.FileList_Font)}};
            Record["colors"] = QJsonObject{{"dialogText",Scheme.Dialog_FGColor.name(QColor::HexRgb)},{"dialogBackground",Scheme.Dialog_BGColor.name(QColor::HexRgb)},{"listText",Scheme.FileList_FGColor.name(QColor::HexRgb)},{"listBackground",Scheme.FileList_BGColor.name(QColor::HexRgb)},{"listCursor",Scheme.FileList_Cursor.name(QColor::HexRgb)},{"listSelection",Scheme.FileList_Selected.name(QColor::HexRgb)}};
            QJsonObject Overrides;
            for (const auto& Binding : ColorBindings)
                Overrides[Binding.key_] = Scheme.explicitColors_.contains(Binding.key_);
            Record["colorOverrides"] = Overrides;
            QJsonObject FileSets;
            for (auto It = Scheme.MapNameToColors.begin(); It != Scheme.MapNameToColors.end(); ++It)
                if (!It.key().isEmpty()) FileSets[It.key()] = QJsonObject{{"foreground",It.value().first.name(QColor::HexRgb)},{"background",It.value().second.name(QColor::HexRgb)}};
            Record["fileSets"] = FileSets; Schemes[Scheme.Name] = Record;
        }
        Appearance["schemes"] = Schemes; Appearance["selected"] = ColorScheme.Name;
        if (!hc::settings::AppSettings().Put(hc::settings::Section::Appearance, Appearance)) return;
    }
    currentColorScheme = Resolved;
    if (!hc::settings::AppSettings().Editing()) emit sigColorSchemeChanged(Resolved);
}

void CColorSchemeMgr::RemoveColorScheme( const QString& Name, bool IsWriteToFile )
{
    if (!mapNameToScheme.contains(Name)) return;
    if (IsWriteToFile) {
        auto Appearance = hc::settings::AppSettings().Read(hc::settings::Section::Appearance).toObject();
        auto Schemes = Appearance["schemes"].toObject(); Schemes.remove(Name); Appearance["schemes"] = Schemes;
        if (Appearance["selected"].toString() == Name) Appearance["selected"] = Schemes.isEmpty() ? QString{} : Schemes.keys().first();
        if (!hc::settings::AppSettings().Put(hc::settings::Section::Appearance, Appearance)) return;
    }
    mapNameToScheme.remove(Name);
    if (currentColorScheme.Name == Name) currentColorScheme = mapNameToScheme.isEmpty() ? TyColorScheme{} : mapNameToScheme.first();
}

void CColorSchemeMgr::UpsertFileSetColor( const QString& ColorScheme, const QString& FileSet, const TyPrFGWithBG& FileSetColor )
{
    Q_ASSERT( mapNameToScheme.contains( ColorScheme ) == true );
    if( mapNameToScheme.contains( ColorScheme ) == false )
        return;

    mapNameToScheme[ ColorScheme ].MapNameToColors[ FileSet ] = FileSetColor;
    if( ColorScheme == GetCurrentColorScheme() )
    {
        currentColorScheme = mapNameToScheme[ ColorScheme ];
        if (!hc::settings::AppSettings().Editing()) emit sigColorSchemeChanged( currentColorScheme );
    }
}

void CColorSchemeMgr::RemoveFileSetColor( const QString& ColorScheme, const QString& FileSet )
{
    Q_ASSERT( mapNameToScheme.contains( ColorScheme ) == true );
    if( mapNameToScheme.contains( ColorScheme ) == false )
        return;

    mapNameToScheme[ ColorScheme ].MapNameToColors.remove( FileSet );
}

bool CColorSchemeMgr::JudgeFileSet( const TyFileSet& FileSet, nsHC::CFileSourceT* Info, QColor* FGColor, QColor* BGColor )
{
    bool IsMatch = false;

    do
    {
        if( Info == nullptr )
            break;

        // Archive, Hidden, Directory, Encrypted, ReadOnly, System, Compressed
        if( FlagOn( FileSet.Flags, FILE_SET_EXP_ATTRIBUTE ) )
        {
            if( FlagOn( FileSet.Attributes, FILE_ATTRIBUTE_ARCHIVE ) &&
                !FlagOn( Info->Attributes_, FILE_ATTRIBUTE_ARCHIVE ) )
                break;

            if( FlagOn( FileSet.Attributes, FILE_ATTRIBUTE_HIDDEN ) &&
                !FlagOn( Info->Attributes_, FILE_ATTRIBUTE_HIDDEN ) )
                break;

            if( FlagOn( FileSet.Attributes, FILE_ATTRIBUTE_DIRECTORY ) &&
                !FlagOn( Info->Attributes_, FILE_ATTRIBUTE_DIRECTORY ) )
                break;

            if( FlagOn( FileSet.Attributes, FILE_ATTRIBUTE_ENCRYPTED ) &&
                !FlagOn( Info->Attributes_, FILE_ATTRIBUTE_ENCRYPTED ) )
                break;

            if( FlagOn( FileSet.Attributes, FILE_ATTRIBUTE_READONLY ) &&
                !FlagOn( Info->Attributes_, FILE_ATTRIBUTE_READONLY ) )
                break;

            if( FlagOn( FileSet.Attributes, FILE_ATTRIBUTE_SYSTEM ) &&
                !FlagOn( Info->Attributes_, FILE_ATTRIBUTE_SYSTEM ) )
                break;

            if( FlagOn( FileSet.Attributes, FILE_ATTRIBUTE_COMPRESSED ) &&
                !FlagOn( Info->Attributes_, FILE_ATTRIBUTE_COMPRESSED ) )
                break;
        }

        // 가장 오래 걸리는 Filters 와 VecExtFilters 를 나중에 검사한다. 

        if( FlagOn( FileSet.Flags, FILE_SET_NOR_EXT_FILTERS ) || FlagOn( FileSet.Flags, FILE_SET_NOR_FILTERS ) )
        {
            bool IsMatchExt = false;
            const auto Ext = Info->Ext_.toLower();

            // 단순 확장자 목록 또는 와일드 카드/정규식을 이용한 표현식 중 하나만 일치하면 된다.
            if( FlagOn( FileSet.Flags, FILE_SET_NOR_EXT_FILTERS ) )
                IsMatchExt |= FileSet.VecExtFilters.contains( Ext );

            //if( IsMatchExt == false && FlagOn( FileSet.Flags, FILE_SET_NOR_FILTERS ) )
            //    ;

            if( IsMatchExt == false )
                break;
        }

        IsMatch = true;

        if( FGColor != nullptr || BGColor != nullptr )
        {
            if( currentColorScheme.MapNameToColors.contains( FileSet.Name ) == true )
            {
                const auto Colors = currentColorScheme.MapNameToColors[ FileSet.Name ];
                if( FGColor != nullptr )
                    *FGColor = Colors.first;
                if( BGColor != nullptr )
                    *BGColor = Colors.second;
            }
        }

    } while( false );

    return IsMatch;
}

QFont CColorSchemeMgr::retrieveFont( const QJsonObject& Record, int DefaultSize ) const
{
    const auto Family = Record["family"].toString("맑은 고딕");
    auto Size = Record["pointSize"].toInt(DefaultSize); if (Size < 6 || Size > 72) Size = DefaultSize;
    QFont Font(Family, Size); Font.setStyleStrategy(QFont::PreferAntialias); Font.setHintingPreference(QFont::PreferFullHinting); return Font;
}
