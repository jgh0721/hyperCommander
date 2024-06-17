#pragma once
#include "cmnConcurrent.hpp"

#include "cmnTypeDefs.hpp"

class CFileSetMgr
{
public:
    void                                Refresh();
    void                                SaveSettings();

    QVector< QString >                  GetNames() const;
    void                                SetFileSet( const TyFileSet& FileSet );
    TyFileSet                           GetFileSet( const QString& FileSetName ) const;
    void                                RemoveFileSet( const QString& FileSetName );

private:

    QMap< QString, TyFileSet >          mapNameToFileSet;
};

using TyStFileSetMgr = nsCmn::nsConcurrent::TSingleton< CFileSetMgr >;
