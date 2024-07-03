#pragma once

#include "cmnConcurrent.hpp"

#include "cmnTypeDefs.hpp"

namespace nsHC
{
    class CFileSourceT;
}

class CColorSchemeMgr : public QObject
{
    Q_OBJECT
public:

    void                                Refresh();
    
    QVector< QString >                  GetNames() const;
    TyColorScheme                       GetColorScheme( const QString& Name ) const;
    QString                             GetCurrentColorScheme() const;
    void                                SetCurrentColorScheme( const QString& Name );
    void                                UpsertColorScheme( const TyColorScheme& ColorScheme, bool IsWriteToFile );
    void                                RemoveColorScheme( const QString& Name, bool IsWriteToFile );

    void                                UpsertFileSetColor( const QString& ColorScheme, const QString& FileSet, const TyPrFGWithBG& FileSetColor );
    void                                RemoveFileSetColor( const QString& ColorScheme, const QString& FileSet );

    //static QString                      JudgeFileSet( const TyFileSet& FileSet, const QString& FileFullPath );
    bool                                JudgeFileSet( const TyFileSet& FileSet, nsHC::CFileSourceT* Info, QColor* FGColor, QColor* BGColor );

signals:

    void                                sigColorSchemeChanged( const TyColorScheme& ColorScheme );
    void                                sigFileSetChanged();

private:
    QFont                               retrieveFont( const QString& FontFamily, const QString& FontSize, int DefaultSize = 9 ) const;

    TyColorScheme                       currentColorScheme;
    QMap< QString, TyColorScheme >      mapNameToScheme;
};

using TyStColorSchemeMgr = nsCmn::nsConcurrent::TSingleton< CColorSchemeMgr >;
