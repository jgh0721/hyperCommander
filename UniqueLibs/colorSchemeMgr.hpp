#pragma once

#include "cmnConcurrent.hpp"

#include "cmnTypeDefs.hpp"

class CColorSchemeMgr
{
public:

    void                                Refresh();
    
    QVector< QString >                  GetNames() const;
    TyColorScheme                       GetColorScheme( const QString& Name ) const;
    void                                SetCurrentColorScheme( const QString& Name );
    void                                UpsertColorScheme( const TyColorScheme& ColorScheme, bool IsWriteToFile );
    void                                RemoveColorScheme( const QString& Name, bool IsWriteToFile );

    void                                UpsertFileSetColor( const QString& ColorScheme, const QString& FileSet, const TyPrFBWithBG& FileSetColor );
    void                                RemoveFileSetColor( const QString& ColorScheme, const QString& FileSet );

    //static QString                      JudgeFileSet( const TyFileSet& FileSet, const QString& FileFullPath );
    bool                                JudgeFileSet( const TyFileSet& FileSet, const Node& Info, QColor* FGColor, QColor* BGColor );

signals:

    void sigColorSchemeChanged( const TyColorScheme& ColorScheme );
    void sigFileSetChanged();

private:
    QFont                               retrieveFont( const QString& FontFamily, const QString& FontSize, int DefaultSize = 9 ) const;

    TyColorScheme                       currentColorScheme;
    QMap< QString, TyColorScheme >      mapNameToScheme;
};

using TyStColorSchemeMgr = nsCmn::nsConcurrent::TSingleton< CColorSchemeMgr >;
