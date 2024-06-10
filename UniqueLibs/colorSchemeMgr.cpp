#include "stdafx.h"
#include "colorSchemeMgr.hpp"

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
        StSettings->beginGroup( QString( "ColorScheme_%1" ).arg( Name ) );
        TyColorScheme ColorScheme;

        ColorScheme.Name = Name;
        ColorScheme.IsDarkMode = StSettings->value( "IsDarkMode" ).toBool();

        QFont Font;

        StSettings->value( "Menu_Font_Family" ).toString();
        StSettings->value( "Menu_Font_Size" ).toInt();

        StSettings->value( "Dialog_Font_Family" ).toString();
        StSettings->value( "Dialog_Font_Size" ).toInt();

        StSettings->value( "List_Font_Family" ).toString();
        StSettings->value( "List_Font_Size" ).toInt();

        StSettings->value( "List_FGColor" ).toString();
        StSettings->value( "List_BGColor" ).toString();
        StSettings->value( "List_CursorColor" ).toString();
        StSettings->value( "List_SelectColor" ).toString();

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
