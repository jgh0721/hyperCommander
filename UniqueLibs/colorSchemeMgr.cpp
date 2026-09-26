#include "stdafx.h"
#include "colorSchemeMgr.hpp"

#include <private/qcssparser_p.h>

#include "cmnTypeDefs_Opts.hpp"
#include "Modules/mdlFileSystem.hpp"

DECLARE_CMNLIBSV2_NAMESPACE

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
        Scheme.IsInverseCursor = Record["inverseCursor"].toBool();
        Scheme.IsInverseSelect = Record["inverseSelection"].toBool();
        const auto Fonts = Record["fonts"].toObject(); const auto Colors = Record["colors"].toObject();
        Scheme.Menu_Font = retrieveFont(Fonts["menu"].toObject(), 9);
        Scheme.Dialog_Font = retrieveFont(Fonts["dialog"].toObject(), 9);
        Scheme.FileList_Font = retrieveFont(Fonts["list"].toObject(), 14);
        Scheme.Dialog_FGColor = QColor::fromString(Colors["dialogText"].toString());
        Scheme.Dialog_BGColor = QColor::fromString(Colors["dialogBackground"].toString());
        Scheme.FileList_FGColor = QColor::fromString(Colors["listText"].toString());
        Scheme.FileList_BGColor = QColor::fromString(Colors["listBackground"].toString());
        Scheme.FileList_Cursor = QColor::fromString(Colors["listCursor"].toString());
        Scheme.FileList_Selected = QColor::fromString(Colors["listSelection"].toString());
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
    mapNameToScheme[ColorScheme.Name] = ColorScheme;
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
            QJsonObject FileSets;
            for (auto It = Scheme.MapNameToColors.begin(); It != Scheme.MapNameToColors.end(); ++It)
                if (!It.key().isEmpty()) FileSets[It.key()] = QJsonObject{{"foreground",It.value().first.name(QColor::HexRgb)},{"background",It.value().second.name(QColor::HexRgb)}};
            Record["fileSets"] = FileSets; Schemes[Scheme.Name] = Record;
        }
        Appearance["schemes"] = Schemes; Appearance["selected"] = ColorScheme.Name;
        if (!hc::settings::AppSettings().Put(hc::settings::Section::Appearance, Appearance)) return;
    }
    currentColorScheme = ColorScheme;
    if (!hc::settings::AppSettings().Editing()) emit sigColorSchemeChanged(ColorScheme);
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
