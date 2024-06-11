#include "stdafx.h"
#include "colorSchemeMgr.hpp"

#include <private/qcssparser_p.h>

#include "cmnTypeDefs_Opts.hpp"


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

        ColorScheme.FileList_FGColor    = QColor::fromString( StSettings->value( "List_FGColor" ).toString() );
        ColorScheme.FileList_BGColor    = QColor::fromString( StSettings->value( "List_BGColor" ).toString() );
        ColorScheme.FileList_Cursor     = QColor::fromString( StSettings->value( "List_CursorColor" ).toString() );
        ColorScheme.FileList_Selected   = QColor::fromString( StSettings->value( "List_SelectColor" ).toString() );

        mapNameToScheme[ Name ] = ColorScheme;
        StSettings->endGroup();
    }

    if( SelectedSchemeName.isEmpty() == true )
        currentColorScheme = mapNameToScheme.value( SelectedSchemeName );

    StSettings->beginGroup( "FileSet" );
    Count = StSettings->value( "Count" ).toInt();
    for( int Idx = 0; Idx < Count; ++Idx )
    {
        TyFileSet FileSet;
        FileSet.Name        = StSettings->value( QString( "%1_Name" ) ).toString();
        FileSet.Flags       = StSettings->value( QString( "%1_Flags" ) ).toUInt();
        
        FileSet.Filters     = StSettings->value( QString( "%1_Filters" ) ).toString().split( '|', Qt::SkipEmptyParts );
        FileSet.VecExtFilters = StSettings->value( QString( "%1_Exts" ) ).toString().split('|', Qt::SkipEmptyParts);
        FileSet.Attributes  = StSettings->value( QString( "%1_Attributes" ) ).toUInt();
        FileSet.Size        = StSettings->value( QString( "%1_Size" ) ).toLongLong();
        mapNameToFileSet[ FileSet.Name ] = FileSet;
    }
    StSettings->endGroup();
}

QVector<QString> CColorSchemeMgr::GetNames() const
{
    return mapNameToScheme.keys();
}

TyColorScheme CColorSchemeMgr::GetColorScheme( const QString& Name ) const
{
    if( mapNameToScheme.contains( Name ) == false )
        return {};

    return mapNameToScheme[ Name ];
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
    if( IsWriteToFile == true )
    {
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

            StSettings->setValue( "List_FGColor",           CS.FileList_FGColor.name( QColor::HexRgb ) );
            StSettings->setValue( "List_BGColor",           CS.FileList_BGColor.name( QColor::HexRgb ) );
            StSettings->setValue( "List_CursorColor",       CS.FileList_Cursor.name( QColor::HexRgb ) );
            StSettings->setValue( "List_SelectColor",       CS.FileList_Selected.name( QColor::HexRgb ) );

            StSettings->endGroup();
        }
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

QVector<QString> CColorSchemeMgr::GetFileSetNames() const
{
    return mapNameToFileSet.keys();
}

TyFileSet CColorSchemeMgr::GetFileSet( const QString& Name ) const
{
    if( mapNameToFileSet.contains( Name ) == false )
        return {};

    return mapNameToFileSet[ Name ];
}

bool CColorSchemeMgr::JudgeFileSet( const TyFileSet& FileSet, const Node& Info )
{
    bool IsMatch = false;

    do
    {
        // Archive, Hidden, Directory, Encrypted, ReadOnly, System, Compressed
        if( FlagOn( FileSet.Flags, FILE_SET_EXP_ATTRIBUTE ) )
        {
            if( FlagOn( FileSet.Attributes, FILE_ATTRIBUTE_ARCHIVE ) &&
                !FlagOn( Info.Attiributes, FILE_ATTRIBUTE_ARCHIVE ) )
                break;

            if( FlagOn( FileSet.Attributes, FILE_ATTRIBUTE_HIDDEN ) &&
                !FlagOn( Info.Attiributes, FILE_ATTRIBUTE_HIDDEN ) )
                break;

            if( FlagOn( FileSet.Attributes, FILE_ATTRIBUTE_DIRECTORY ) &&
                !FlagOn( Info.Attiributes, FILE_ATTRIBUTE_DIRECTORY ) )
                break;

            if( FlagOn( FileSet.Attributes, FILE_ATTRIBUTE_ENCRYPTED ) &&
                !FlagOn( Info.Attiributes, FILE_ATTRIBUTE_ENCRYPTED ) )
                break;

            if( FlagOn( FileSet.Attributes, FILE_ATTRIBUTE_READONLY ) &&
                !FlagOn( Info.Attiributes, FILE_ATTRIBUTE_READONLY ) )
                break;

            if( FlagOn( FileSet.Attributes, FILE_ATTRIBUTE_SYSTEM ) &&
                !FlagOn( Info.Attiributes, FILE_ATTRIBUTE_SYSTEM ) )
                break;

            if( FlagOn( FileSet.Attributes, FILE_ATTRIBUTE_COMPRESSED ) &&
                !FlagOn( Info.Attiributes, FILE_ATTRIBUTE_COMPRESSED ) )
                break;
        }

        // 가장 오래 걸리는 Filters 와 VecExtFilters 를 나중에 검사한다. 
        if( FlagOn( FileSet.Flags, FILE_SET_NOR_EXT_FILTERS ) )
        {
            Info.Ext;
            FileSet.VecExtFilters;

        }

        IsMatch = true;

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
