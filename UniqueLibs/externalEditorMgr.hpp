#pragma once

#include "cmnConcurrent.hpp"

#include "UniqueLibs/cmnTypeDefs.hpp"


/*!
    [Editor]
    Count=0
    NameN=
    ProgramN=
    IconN=
    CMDLineN=
    DetectN=
    ExtsN=


*/

class CExternalEditorMgr : public QObject
{
    Q_OBJECT
public:
    void                                Refresh();
    QVector< TyExternalEditor >         RetrieveSettings() const;

    void                                ConstructExternalMenu( QMenu* Menu, const QString& FileFullPath );
    
    void                                AddItem( const TyExternalEditor& Item );
    void                                RemoveItem( int Index );
    void                                ModifyItem( int Index, const TyExternalEditor& Item );

protected slots:
    
    void                                ac_configure();

private:

    void                                saveSettings();

    QVector< TyExternalEditor >         vecExternalItems;
};

using TyStExternalEditorMgr = nsCmn::nsConcurrent::TSingleton<CExternalEditorMgr>;
