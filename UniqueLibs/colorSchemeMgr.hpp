#pragma once

#include "cmnConcurrent.hpp"

#include "cmnTypeDefs.hpp"

/*!
    Index => 0..N

    [ColorScheme]
    Count=N
    Selected=N
    N_Name=XXXX

    [ColorScheme_XXXX]
    IsDarkMode=false

    Menu_Font_Family=
    Menu_Font_Size=Npt or Npx         
    Dialog_Font_Family=
    Dialog_Font_Size=Npt or Npx
    List_Font_Family=
    List_Font_Size=Npt or Npx
    List_FGColor=
    List_BGColor=
    List_CursorColor=
    List_SelectColor=

    FileSet_XXX_FGColor=
    FileSet_XXX_BGColor=
        Color => #RGB, #RRGGBB, #AARRGGBB, #RRRGGGBBB, #RRRRGGGGBBBB, SVG Color Keyword

    [FileSet]
    Count=N
    0_Name=XXXX
    0_Flags=XXXX
    0_Filters=X
    0_Exts=XXX|XXX
    0_Attributes=Z
    0_Size=Z
 */

class CColorSchemeMgr
{
public:

    void                                Refresh();

    QVector< QString >                  GetNames() const;
    TyColorScheme                       GetColorScheme( const QString& Name ) const;
    void                                SetCurrentColorScheme( const QString& Name );

    QVector< QString >                  GetFileSetNames() const;
    TyFileSet                           GetFileSet( const QString& Name ) const;
    static QString                      JudgeFileSet( const TyFileSet& FileSet, const QString& FileFullPath );
    static bool                         JudgeFileSet( const TyFileSet& FileSet, const Node& Info );

signals:

    void sigColorSchemeChanged( const TyColorScheme& ColorScheme );
    void sigFileSetChanged();

private:

    TyColorScheme                       currentColorScheme;
    QMap< QString, TyColorScheme >      mapNameToScheme;
    QMap< QString, TyFileSet>           mapNameToFileSet;
};

using TyStColorSchemeMgr = nsCmn::nsConcurrent::TSingleton< CColorSchemeMgr >;
