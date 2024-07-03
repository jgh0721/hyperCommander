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
    QSet< QString > SetSchemeNames;

    StSettings->beginGroup( "ColorScheme" );

    auto Count = StSettings->value( "Count" ).toInt();
    const auto Selected = StSettings->value( "Selected" ).toInt();
    QString SelectedSchemeName;

    for( int Idx = 0; Idx < Count; ++Idx )
    {
        const auto Name = StSettings->value( QString( "%1_Name" ).arg( Idx ) ).toString();
        if( Selected == Idx )
            SelectedSchemeName = Name;
        SetSchemeNames.insert( Name );
    }

    StSettings->endGroup();

    for( const auto& Name : SetSchemeNames )
    {
        TyColorScheme ColorScheme;
        StSettings->beginGroup( QString( "ColorScheme_%1" ).arg( Name ) );

        ColorScheme.Name                = Name;
        ColorScheme.IsDarkMode          = StSettings->value( "IsDarkMode" ).toBool();

        ColorScheme.Menu_Font           = retrieveFont( "Menu_Font_Family", "Menu_Font_Size" );
        ColorScheme.Dialog_Font         = retrieveFont( "Dialog_Font_Family", "Dialog_Font_Size" );
        ColorScheme.FileList_Font       = retrieveFont( "List_Font_Family", "List_Font_Size", 14 );

        ColorScheme.Dialog_FGColor      = QColor::fromString( StSettings->value( "Dialog_FGColor" ).toString() );
        ColorScheme.Dialog_BGColor      = QColor::fromString( StSettings->value( "Dialog_BGColor" ).toString() );

        ColorScheme.FileList_FGColor    = QColor::fromString( StSettings->value( "List_FGColor" ).toString() );
        ColorScheme.FileList_BGColor    = QColor::fromString( StSettings->value( "List_BGColor" ).toString() );
        ColorScheme.FileList_Cursor     = QColor::fromString( StSettings->value( "List_CursorColor" ).toString() );
        ColorScheme.FileList_Selected   = QColor::fromString( StSettings->value( "List_SelectColor" ).toString() );

        for( const auto& Child : StSettings->childKeys() )
        {
            if( Child.startsWith( "FileSet", Qt::CaseInsensitive ) == false )
                continue;

            // FileSet_XXX_FGColor or FileSet_XXX_BGColor
            const auto L = Child.split( '_', Qt::SkipEmptyParts );
            if( L.size() != 3 )
                continue;

            const auto FileSetName = L[ 1 ];
            if( Child.contains( "FGColor", Qt::CaseInsensitive ) == true )
                ColorScheme.MapNameToColors[ FileSetName ].first = QColor::fromString( StSettings->value( Child ).toString() );
            if( Child.contains( "BGColor", Qt::CaseInsensitive ) == true )
                ColorScheme.MapNameToColors[ FileSetName ].second = QColor::fromString( StSettings->value( Child ).toString() );
        }

        mapNameToScheme[ Name ] = ColorScheme;
        StSettings->endGroup();
    }

    if( SelectedSchemeName.isEmpty() == false )
        currentColorScheme = mapNameToScheme.value( SelectedSchemeName );
    else
    {
        if( mapNameToScheme.isEmpty() == false )
            currentColorScheme = mapNameToScheme.first();
    }
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
    mapNameToScheme[ ColorScheme.Name ] = ColorScheme;
    if( IsWriteToFile == false )
        return;

    StSettings->beginGroup( "ColorScheme" );
    StSettings->setValue( "Count", mapNameToScheme.count() );
    bool IsAlready = false;
    for( int Idx = 0; Idx < mapNameToScheme.count(); ++Idx )
    {
        if( ColorScheme.Name.compare( StSettings->value( QString( "%1_Name" ).arg( Idx ) ).toString(), Qt::CaseInsensitive ) != 0 )
            continue;

        IsAlready = true;
        break;
    }
    if( IsAlready == false )
        StSettings->setValue( QString( "%1_Name" ).arg( mapNameToScheme.count() - 1 ), ColorScheme.Name );
    StSettings->endGroup();

    for( const auto& CS : mapNameToScheme )
    {
        StSettings->beginGroup( QString( "ColorScheme_%1" ).arg( CS.Name ) );

        StSettings->setValue( "IsDarkMode",             CS.IsDarkMode );
        StSettings->setValue( "Menu_Font_Family",       CS.Menu_Font.family() );
        StSettings->setValue( "Menu_Font_Size",         CS.Menu_Font.pointSize() );

        StSettings->setValue( "Dialog_Font_Family",     CS.Dialog_Font.family() );
        StSettings->setValue( "Dialog_Font_Size",       CS.Dialog_Font.pointSize() );
        
        StSettings->setValue( "List_Font_Family",       CS.FileList_Font.family() );
        StSettings->setValue( "List_Font_Size",         CS.FileList_Font.pointSize() );

        StSettings->setValue( "Dialog_FGColor",         CS.Dialog_FGColor.name( QColor::HexRgb ) );
        StSettings->setValue( "Dialog_BGColor",         CS.Dialog_BGColor.name( QColor::HexRgb ) );

        StSettings->setValue( "List_FGColor",           CS.FileList_FGColor.name( QColor::HexRgb ) );
        StSettings->setValue( "List_BGColor",           CS.FileList_BGColor.name( QColor::HexRgb ) );
        StSettings->setValue( "List_CursorColor",       CS.FileList_Cursor.name( QColor::HexRgb ) );
        StSettings->setValue( "List_SelectColor",       CS.FileList_Selected.name( QColor::HexRgb ) );

        for( const auto& FileSet : CS.MapNameToColors.keys() )
        {
            StSettings->setValue( QString( "FileSet_%1_FGColor" ).arg( FileSet ), CS.MapNameToColors[ FileSet ].first.name( QColor::HexRgb ) );
            StSettings->setValue( QString( "FileSet_%1_BGColor" ).arg( FileSet ), CS.MapNameToColors[ FileSet ].second.name( QColor::HexRgb ) );
        }
        
        StSettings->endGroup();
    }

    if( ColorScheme.Name == currentColorScheme.Name )
    {
        currentColorScheme = ColorScheme;
        emit sigColorSchemeChanged( ColorScheme );
    }
}

void CColorSchemeMgr::RemoveColorScheme( const QString& Name, bool IsWriteToFile )
{
    if( mapNameToScheme.contains( Name ) == false )
        return;

    // TODO: 하나 있는 것을 삭제하면 어떻게 할 것인지 고민해보자. 
    if( mapNameToScheme.count() == 1 )
    {
        Q_ASSERT( false );
    }

    const auto PrevCount = mapNameToScheme.count();
    mapNameToScheme.remove( Name );

    if( IsWriteToFile == true )
    {
        StSettings->beginGroup( "ColorScheme" );
        for( int Idx = 0; Idx < mapNameToScheme.count(); ++Idx )
        {
            const auto Key = QString( "%1_Name" ).arg( Idx );
            if( StSettings->value( Key ).toString().compare( Name, Qt::CaseInsensitive ) != 0 )
                continue;

            StSettings->remove( Key );
            break;
        }
        StSettings->setValue( "Count", mapNameToScheme.count() );
        StSettings->endGroup();

        StSettings->beginGroup( QString( "ColorScheme_%1" ).arg( Name ) );
        StSettings->remove( "" );
        StSettings->endGroup();
    }
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
        emit sigColorSchemeChanged( currentColorScheme );
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

QFont CColorSchemeMgr::retrieveFont( const QString& FontFamily, const QString& FontSize, int DefaultSize ) const
{
    // TODO: dlgOpts 에서 설정되는 기본 글꼴 값과 일치해야 한다. 
    auto Family = StSettings->value( FontFamily ).toString();
    if( Family.isEmpty() == true )
        Family = "맑은 고딕";

    auto Size = StSettings->value( FontSize ).toInt();
    if( Size <= 0 )
        Size = DefaultSize;

    QFont Font( Family, Size );
    Font.setStyleStrategy( QFont::PreferAntialias );
    Font.setHintingPreference( QFont::PreferFullHinting );
    return Font;
}
